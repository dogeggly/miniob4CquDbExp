/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "common/lang/utility.h"
#include "common/lang/vector.h"
#include "storage/index/index.h"

/**
 * @brief IVF Flat 倒排列表中的条目
 * @ingroup Index
 * @details 存储向量数据（float数组）和对应的 RID
 */
struct IvfflatListItem
{
  vector<float> vec;  ///< 向量数据
  RID           rid;  ///< 记录位置
};

/**
 * @brief IVF Flat 向量索引扫描器
 * @ingroup Index
 */
class IvfflatIndexScanner : public IndexScanner
{
public:
  IvfflatIndexScanner(int dim, const vector<float> &query, const vector<vector<float>> &centroids,
      const vector<vector<IvfflatListItem>> &inverted_lists, int probes, const string &distance_method);
  ~IvfflatIndexScanner() override = default;

  RC next_entry(RID *rid) override;
  RC destroy() override;

private:
  /// 计算查询向量与质心之间的距离
  float compute_distance(const float *a, const float *b, int dim);

private:
  int dim_;  ///< 向量维度

  /// 按距离排序的 (距离, 簇ID) 对
  vector<pair<float, int>> cluster_distances_;

  /// 引用倒排列表（不持有所有权）
  const vector<vector<IvfflatListItem>> &inverted_lists_;

  /// 当前扫描状态
  int    current_probe_  = 0;    ///< 当前正在扫描第几个探测簇
  size_t current_offset_ = 0;    ///< 当前簇内的偏移量
  int    probes_;                 ///< 探测簇数

  string distance_method_;
};

/**
 * @brief IVF Flat 向量索引
 * @ingroup Index
 * @details 实现 IVF (Inverted File) Flat 索引：
 * - 训练阶段使用 K-Means 聚类将向量空间划分为 lists 个区域
 * - 每个区域（簇）维护一个倒排列表，存储属于该簇的所有向量及其 RID
 * - 查询时计算查询向量与所有质心的距离，选择最近的 probes 个簇进行扫描
 *
 * 使用方式：
 * 1. 创建索引：CREATE VECTOR INDEX idx ON t(col) WITH (type=ivfflat, distance=cosine, lists=10, probes=3)
 * 2. 查询时优化器会自动识别 IVF 索引并使用 ANN 扫描
 */
class IvfflatIndex : public Index
{
public:
  IvfflatIndex() = default;
  virtual ~IvfflatIndex() noexcept;

  bool is_vector_index() override { return true; }

  RC create(Table *table, const char *file_name, const IndexMeta &index_meta, const FieldMeta &field_meta) override;
  RC open(Table *table, const char *file_name, const IndexMeta &index_meta, const FieldMeta &field_meta) override;
  RC close();

  RC insert_entry(const char *record, const RID *rid) override;
  RC delete_entry(const char *record, const RID *rid) override;

  IndexScanner *create_scanner(const char *left_key, int left_len, bool left_inclusive, const char *right_key,
      int right_len, bool right_inclusive) override;

  RC sync() override;

  /// 近似最近邻搜索：返回按距离排序的 RID 列表
  vector<RID> ann_search(const vector<float> &query, size_t limit);

private:
  /// K-Means 训练：从数据中学习 lists 个聚类中心
  RC train_kmeans(const vector<vector<float>> &all_vectors, int lists, int max_iterations = 20);

  /// 为单个向量找到最近的质心索引
  int  find_nearest_centroid(const float *vec, int dim);
  /// 找到距离查询向量最近的 probes 个质心索引
  vector<int> find_nearest_centroids(const float *vec, int dim, int probes);

  /// 从完整记录中提取向量数据
  vector<float> extract_vector(const char *record);

  /// 计算两点间的距离
  float compute_distance(const float *a, const float *b, int dim);

  /// 将索引持久化到文件
  RC save_to_file(const char *file_name);
  /// 从文件加载索引
  RC load_from_file(const char *file_name);

private:
  bool   inited_ = false;
  Table *table_  = nullptr;
  int    dim_    = 0;  ///< 向量维度

  /// K-Means 聚类中心（lists 个中心向量）
  vector<vector<float>> centroids_;

  /// 倒排列表：第 i 个列表对应第 i 个质心
  vector<vector<IvfflatListItem>> inverted_lists_;

  /// 索引属性
  string distance_method_;  ///< 距离度量方法
  int    lists_  = 1;       ///< 聚类数
  int    probes_ = 1;       ///< 搜索探测数
};
