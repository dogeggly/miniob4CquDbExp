/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "storage/index/ivfflat_index.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

#include "common/lang/fstream.h"
#include "common/log/log.h"
#include "storage/record/record_manager.h"
#include "storage/record/record_scanner.h"
#include "storage/table/table.h"

using namespace std;

// ============================================================================
// IVF Flat IndexScanner 实现
// ============================================================================

IvfflatIndexScanner::IvfflatIndexScanner(int dim, const vector<float> &query, const vector<vector<float>> &centroids,
    const vector<vector<IvfflatListItem>> &inverted_lists, int probes, const string &distance_method)
    : dim_(dim), inverted_lists_(inverted_lists), probes_(probes), distance_method_(distance_method)
{
  // 1. 计算查询向量与所有质心的距离
  for (size_t i = 0; i < centroids.size(); i++) {
    float dist = compute_distance(query.data(), centroids[i].data(), dim);
    cluster_distances_.emplace_back(dist, static_cast<int>(i));
  }

  // 2. 按距离升序排序，选前 probes 个簇
  sort(cluster_distances_.begin(), cluster_distances_.end(),
      [](const pair<float, int> &a, const pair<float, int> &b) { return a.first < b.first; });

  // 截断至 probes 个
  if (static_cast<int>(cluster_distances_.size()) > probes_) {
    cluster_distances_.resize(probes_);
  }
}

float IvfflatIndexScanner::compute_distance(const float *a, const float *b, int dim)
{
  if (distance_method_ == "cosine") {
    // 余弦距离: 1 - cos_similarity
    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (int i = 0; i < dim; i++) {
      dot   += a[i] * b[i];
      norm_a += a[i] * a[i];
      norm_b += b[i] * b[i];
    }
    if (norm_a < 1e-8f || norm_b < 1e-8f) {
      return numeric_limits<float>::max();
    }
    float cos_sim = dot / (sqrtf(norm_a) * sqrtf(norm_b));
    // clamp to [-1, 1]
    cos_sim = max(-1.0f, min(1.0f, cos_sim));
    return 1.0f - cos_sim;
  } else if (distance_method_ == "dot") {
    // 点积距离: -dot_product
    float dot = 0.0f;
    for (int i = 0; i < dim; i++) {
      dot += a[i] * b[i];
    }
    return -dot;
  } else {
    // 欧几里得距离平方 (默认)
    float sum = 0.0f;
    for (int i = 0; i < dim; i++) {
      float diff = a[i] - b[i];
      sum += diff * diff;
    }
    return sum;
  }
}

RC IvfflatIndexScanner::next_entry(RID *rid)
{
  // 遍历选中的 probes 个簇的倒排列表
  while (current_probe_ < probes_ && current_probe_ < static_cast<int>(cluster_distances_.size())) {
    int cluster_id = cluster_distances_[current_probe_].second;

    // 检查簇索引有效性
    if (cluster_id < 0 || cluster_id >= static_cast<int>(inverted_lists_.size())) {
      current_probe_++;
      current_offset_ = 0;
      continue;
    }

    const auto &list = inverted_lists_[cluster_id];

    if (current_offset_ < list.size()) {
      *rid = list[current_offset_].rid;
      current_offset_++;
      return RC::SUCCESS;
    } else {
      // 当前簇已遍历完毕，移到下一个簇
      current_probe_++;
      current_offset_ = 0;
    }
  }

  return RC::RECORD_EOF;
}

RC IvfflatIndexScanner::destroy()
{
  delete this;
  return RC::SUCCESS;
}

// ============================================================================
// IVF Flat Index 实现
// ============================================================================

IvfflatIndex::~IvfflatIndex() noexcept { close(); }

RC IvfflatIndex::create(Table *table, const char *file_name, const IndexMeta &index_meta, const FieldMeta &field_meta)
{
  if (inited_) {
    LOG_WARN("IvfflatIndex already created");
    return RC::INTERNAL;
  }

  // 仅支持 VECTOR 类型
  if (field_meta.type() != AttrType::VECTORS) {
    LOG_ERROR("IvfflatIndex only supports VECTOR type, but got %d", static_cast<int>(field_meta.type()));
    return RC::INVALID_ARGUMENT;
  }

  // 保存属性
  RC rc = Index::init(index_meta, field_meta);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  table_ = table;
  dim_   = field_meta.len() / sizeof(float);

  distance_method_ = index_meta.distance_method();
  lists_           = index_meta.lists();
  probes_          = index_meta.probes();

  // 验证参数
  if (lists_ < 1) {
    LOG_ERROR("IVF lists must be >= 1, got %d", lists_);
    return RC::INVALID_ARGUMENT;
  }
  if (probes_ < 1) {
    probes_ = 1;
  }
  if (probes_ > lists_) {
    probes_ = lists_;
  }

  // 距离度量默认值
  if (distance_method_.empty()) {
    distance_method_ = "euclidean";
  }

  // 1. 扫描表中所有记录，收集向量数据
  vector<vector<float>> all_vectors;
  vector<RID>           all_rids;

  RecordScanner *scanner = nullptr;
  rc = table_->get_record_scanner(scanner, nullptr, ReadWriteMode::READ_ONLY);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to get record scanner for IVF index training");
    return rc;
  }

  Record record;
  while (OB_SUCC(rc = scanner->next(record))) {
    vector<float> vec = extract_vector(record.data());
    if (!vec.empty()) {
      all_vectors.push_back(std::move(vec));
      all_rids.push_back(record.rid());
    }
  }

  if (RC::RECORD_EOF == rc) {
    rc = RC::SUCCESS;
  }
  scanner->close_scan();
  delete scanner;

  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to scan records for IVF index training");
    return rc;
  }

  if (all_vectors.empty()) {
    LOG_INFO("No data for IVF index training, initializing empty centroids");
    // 使用随机初始化创建空质心
    centroids_.resize(lists_);
    for (int i = 0; i < lists_; i++) {
      centroids_[i].assign(dim_, 0.0f);
    }
    inverted_lists_.resize(lists_);
    inited_ = true;
    return save_to_file(file_name);
  }

  LOG_INFO("Collected %zu vectors for IVF index training, dim=%d, lists=%d", all_vectors.size(), dim_, lists_);

  // 调整 lists 不超过数据量
  if (lists_ > static_cast<int>(all_vectors.size())) {
    LOG_INFO("Reducing lists from %d to %zu (data size)", lists_, all_vectors.size());
    lists_ = static_cast<int>(all_vectors.size());
  }

  // 2. K-Means 训练
  rc = train_kmeans(all_vectors, lists_);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to train K-Means for IVF index");
    return rc;
  }

  // 3. 构建倒排列表
  inverted_lists_.clear();
  inverted_lists_.resize(lists_);

  for (size_t i = 0; i < all_vectors.size(); i++) {
    int cluster_id = find_nearest_centroid(all_vectors[i].data(), dim_);
    if (cluster_id < 0 || cluster_id >= lists_) {
      cluster_id = 0;
    }
    inverted_lists_[cluster_id].push_back({all_vectors[i], all_rids[i]});
  }

  LOG_INFO("IVF index created: lists=%d, total vectors=%zu", lists_, all_vectors.size());
  for (int i = 0; i < lists_; i++) {
    LOG_INFO("  cluster[%d]: %zu vectors", i, inverted_lists_[i].size());
  }

  inited_ = true;

  // 4. 持久化到文件
  return save_to_file(file_name);
}

RC IvfflatIndex::open(Table *table, const char *file_name, const IndexMeta &index_meta, const FieldMeta &field_meta)
{
  if (inited_) {
    LOG_WARN("IvfflatIndex already opened");
    return RC::INTERNAL;
  }

  RC rc = Index::init(index_meta, field_meta);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  table_           = table;
  dim_             = field_meta.len() / sizeof(float);
  distance_method_ = index_meta.distance_method();
  lists_           = index_meta.lists();
  probes_          = index_meta.probes();

  if (probes_ < 1) {
    probes_ = 1;
  }
  if (distance_method_.empty()) {
    distance_method_ = "euclidean";
  }

  inited_ = true;

  return load_from_file(file_name);
}

RC IvfflatIndex::close()
{
  inited_ = false;
  table_  = nullptr;
  centroids_.clear();
  inverted_lists_.clear();
  return RC::SUCCESS;
}

RC IvfflatIndex::insert_entry(const char *record, const RID *rid)
{
  if (!inited_) {
    return RC::INTERNAL;
  }

  vector<float> vec = extract_vector(record);
  if (vec.empty()) {
    return RC::INVALID_ARGUMENT;
  }

  int cluster_id = find_nearest_centroid(vec.data(), dim_);
  if (cluster_id < 0 || cluster_id >= static_cast<int>(inverted_lists_.size())) {
    cluster_id = 0;
  }

  inverted_lists_[cluster_id].push_back({std::move(vec), *rid});
  return RC::SUCCESS;
}

RC IvfflatIndex::delete_entry(const char *record, const RID *rid)
{
  if (!inited_) {
    return RC::INTERNAL;
  }

  vector<float> vec = extract_vector(record);
  if (vec.empty()) {
    return RC::INVALID_ARGUMENT;
  }

  int cluster_id = find_nearest_centroid(vec.data(), dim_);
  if (cluster_id < 0 || cluster_id >= static_cast<int>(inverted_lists_.size())) {
    return RC::INVALID_ARGUMENT;
  }

  auto &list = inverted_lists_[cluster_id];
  for (auto it = list.begin(); it != list.end(); ++it) {
    if (it->rid == *rid) {
      list.erase(it);
      return RC::SUCCESS;
    }
  }

  LOG_WARN("Entry not found in IVF index for deletion: page=%d, slot=%d", rid->page_num, rid->slot_num);
  return RC::RECORD_NOT_EXIST;
}

IndexScanner *IvfflatIndex::create_scanner(const char *left_key, int left_len, bool left_inclusive,
    const char *right_key, int right_len, bool right_inclusive)
{
  // IVF Flat 索引使用 ANN 搜索，不接受传统范围查询
  // 此方法为兼容 Index 基类接口，实际使用请通过 ann_search()
  return nullptr;
}

RC IvfflatIndex::sync()
{
  // 内存索引，需要通过 save_to_file 持久化
  return RC::SUCCESS;
}

vector<RID> IvfflatIndex::ann_search(const vector<float> &query, size_t limit)
{
  if (!inited_ || centroids_.empty()) {
    return {};
  }

  if (static_cast<int>(query.size()) != dim_) {
    LOG_WARN("Query dimension %zu doesn't match index dimension %d", query.size(), dim_);
    return {};
  }

  // 1. 找到最近的 probes 个质心
  vector<int> nearest_clusters = find_nearest_centroids(query.data(), dim_, probes_);

  // 2. 收集候选结果：扫描选中簇的所有向量，计算距离
  struct Candidate
  {
    RID   rid;
    float distance;
  };
  vector<Candidate> candidates;

  for (int cluster_id : nearest_clusters) {
    if (cluster_id < 0 || cluster_id >= static_cast<int>(inverted_lists_.size())) {
      continue;
    }
    for (const auto &item : inverted_lists_[cluster_id]) {
      float dist = compute_distance(query.data(), item.vec.data(), dim_);
      candidates.push_back({item.rid, dist});
    }
  }

  // 3. 按距离排序
  sort(candidates.begin(), candidates.end(),
      [](const Candidate &a, const Candidate &b) { return a.distance < b.distance; });

  // 4. 截断至 limit
  if (candidates.size() > limit) {
    candidates.resize(limit);
  }

  // 5. 提取 RID 列表
  vector<RID> result;
  result.reserve(candidates.size());
  for (const auto &c : candidates) {
    result.push_back(c.rid);
  }

  return result;
}

// ============================================================================
// K-Means 聚类训练
// ============================================================================

RC IvfflatIndex::train_kmeans(const vector<vector<float>> &all_vectors, int lists, int max_iterations)
{
  if (all_vectors.empty() || lists <= 0) {
    return RC::INVALID_ARGUMENT;
  }

  int data_size = static_cast<int>(all_vectors.size());

  // 1. 随机初始化质心（从数据点中随机选择）
  centroids_.clear();
  centroids_.resize(lists);

  // 使用随机数选择初始质心
  random_device              rd;
  mt19937                    gen(rd());
  uniform_int_distribution<> dis(0, data_size - 1);

  for (int i = 0; i < lists; i++) {
    int idx = dis(gen);
    centroids_[i] = all_vectors[idx];
  }

  // 2. 迭代优化
  vector<int> assignments(data_size, 0);  // 每个数据点的簇分配

  for (int iter = 0; iter < max_iterations; iter++) {
    // 2a. 分配步骤：将每个数据点分配到最近的质心
    bool changed = false;
    for (int i = 0; i < data_size; i++) {
      int new_cluster = find_nearest_centroid(all_vectors[i].data(), dim_);
      if (new_cluster != assignments[i]) {
        assignments[i] = new_cluster;
        changed        = true;
      }
    }

    if (!changed) {
      LOG_INFO("K-Means converged after %d iterations", iter + 1);
      break;
    }

    // 2b. 更新步骤：重新计算每个簇的质心（均值）
    vector<vector<float>> new_centroids(lists, vector<float>(dim_, 0.0f));
    vector<int>           counts(lists, 0);

    for (int i = 0; i < data_size; i++) {
      int c = assignments[i];
      if (c < 0 || c >= lists) {
        continue;
      }
      for (int j = 0; j < dim_; j++) {
        new_centroids[c][j] += all_vectors[i][j];
      }
      counts[c]++;
    }

    // 更新质心
    for (int c = 0; c < lists; c++) {
      if (counts[c] > 0) {
        for (int j = 0; j < dim_; j++) {
          centroids_[c][j] = new_centroids[c][j] / counts[c];
        }
      }
      // 如果某个簇没有数据点，随机重新初始化
      else {
        int idx = dis(gen);
        centroids_[c] = all_vectors[idx];
      }
    }

    LOG_TRACE("K-Means iteration %d completed", iter + 1);
  }

  return RC::SUCCESS;
}

int IvfflatIndex::find_nearest_centroid(const float *vec, int dim)
{
  if (centroids_.empty()) {
    return 0;
  }

  int   best_id = -1;
  float best_dist = numeric_limits<float>::max();

  for (size_t i = 0; i < centroids_.size(); i++) {
    float dist = compute_distance(vec, centroids_[i].data(), dim);
    if (dist < best_dist) {
      best_dist = dist;
      best_id   = static_cast<int>(i);
    }
  }

  return best_id;
}

vector<int> IvfflatIndex::find_nearest_centroids(const float *vec, int dim, int probes)
{
  if (centroids_.empty()) {
    return {};
  }

  // 计算所有质心距离
  vector<pair<float, int>> distances;
  distances.reserve(centroids_.size());

  for (size_t i = 0; i < centroids_.size(); i++) {
    float dist = compute_distance(vec, centroids_[i].data(), dim);
    distances.emplace_back(dist, static_cast<int>(i));
  }

  // 按距离升序排序
  sort(distances.begin(), distances.end(),
      [](const pair<float, int> &a, const pair<float, int> &b) { return a.first < b.first; });

  // 选前 probes 个
  int actual_probes = min(probes, static_cast<int>(distances.size()));
  vector<int> result;
  result.reserve(actual_probes);
  for (int i = 0; i < actual_probes; i++) {
    result.push_back(distances[i].second);
  }

  return result;
}

vector<float> IvfflatIndex::extract_vector(const char *record)
{
  // 根据字段偏移量提取向量数据
  const float *data = reinterpret_cast<const float *>(record + field_meta_.offset());
  return vector<float>(data, data + dim_);
}

float IvfflatIndex::compute_distance(const float *a, const float *b, int dim)
{
  if (distance_method_ == "cosine") {
    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (int i = 0; i < dim; i++) {
      dot   += a[i] * b[i];
      norm_a += a[i] * a[i];
      norm_b += b[i] * b[i];
    }
    if (norm_a < 1e-8f || norm_b < 1e-8f) {
      return numeric_limits<float>::max();
    }
    float cos_sim = dot / (sqrtf(norm_a) * sqrtf(norm_b));
    cos_sim = max(-1.0f, min(1.0f, cos_sim));
    return 1.0f - cos_sim;
  } else if (distance_method_ == "dot") {
    float dot = 0.0f;
    for (int i = 0; i < dim; i++) {
      dot += a[i] * b[i];
    }
    return -dot;
  } else {
    // 欧几里得距离平方（默认）
    float sum = 0.0f;
    for (int i = 0; i < dim; i++) {
      float diff = a[i] - b[i];
      sum += diff * diff;
    }
    return sum;
  }
}

// ============================================================================
// 持久化 (简化二进制格式)
// ============================================================================

RC IvfflatIndex::save_to_file(const char *file_name)
{
  ofstream ofs(file_name, ios::binary | ios::trunc);
  if (!ofs.is_open()) {
    LOG_ERROR("Failed to open file for writing: %s", file_name);
    return RC::IOERR_OPEN;
  }

  // Header: dim, lists, probes
  ofs.write(reinterpret_cast<const char *>(&dim_), sizeof(dim_));
  ofs.write(reinterpret_cast<const char *>(&lists_), sizeof(lists_));
  ofs.write(reinterpret_cast<const char *>(&probes_), sizeof(probes_));

  // 距离方法名字符串
  size_t method_len = distance_method_.size();
  ofs.write(reinterpret_cast<const char *>(&method_len), sizeof(method_len));
  ofs.write(distance_method_.data(), static_cast<streamsize>(method_len));

  // 质心数据
  for (const auto &centroid : centroids_) {
    ofs.write(reinterpret_cast<const char *>(centroid.data()), static_cast<streamsize>(dim_ * sizeof(float)));
  }

  // 倒排列表
  for (const auto &list : inverted_lists_) {
    size_t list_size = list.size();
    ofs.write(reinterpret_cast<const char *>(&list_size), sizeof(list_size));
    for (const auto &item : list) {
      ofs.write(reinterpret_cast<const char *>(item.vec.data()), static_cast<streamsize>(dim_ * sizeof(float)));
      ofs.write(reinterpret_cast<const char *>(&item.rid), sizeof(RID));
    }
  }

  ofs.close();
  LOG_INFO("IVF index saved to %s", file_name);
  return RC::SUCCESS;
}

RC IvfflatIndex::load_from_file(const char *file_name)
{
  ifstream ifs(file_name, ios::binary);
  if (!ifs.is_open()) {
    LOG_ERROR("Failed to open file for reading: %s", file_name);
    return RC::IOERR_OPEN;
  }

  // Header
  ifs.read(reinterpret_cast<char *>(&dim_), sizeof(dim_));
  ifs.read(reinterpret_cast<char *>(&lists_), sizeof(lists_));
  ifs.read(reinterpret_cast<char *>(&probes_), sizeof(probes_));

  // 距离方法名
  size_t method_len = 0;
  ifs.read(reinterpret_cast<char *>(&method_len), sizeof(method_len));
  distance_method_.resize(method_len);
  ifs.read(&distance_method_[0], static_cast<streamsize>(method_len));

  // 质心数据
  centroids_.resize(lists_);
  for (int i = 0; i < lists_; i++) {
    centroids_[i].resize(dim_);
    ifs.read(reinterpret_cast<char *>(centroids_[i].data()), static_cast<streamsize>(dim_ * sizeof(float)));
  }

  // 倒排列表
  inverted_lists_.resize(lists_);
  for (int i = 0; i < lists_; i++) {
    size_t list_size = 0;
    ifs.read(reinterpret_cast<char *>(&list_size), sizeof(list_size));
    inverted_lists_[i].resize(list_size);
    for (size_t j = 0; j < list_size; j++) {
      inverted_lists_[i][j].vec.resize(dim_);
      ifs.read(reinterpret_cast<char *>(inverted_lists_[i][j].vec.data()),
          static_cast<streamsize>(dim_ * sizeof(float)));
      ifs.read(reinterpret_cast<char *>(&inverted_lists_[i][j].rid), sizeof(RID));
    }
  }

  ifs.close();
  LOG_INFO("IVF index loaded from %s: dim=%d, lists=%d, probes=%d", file_name, dim_, lists_, probes_);
  return RC::SUCCESS;
}
