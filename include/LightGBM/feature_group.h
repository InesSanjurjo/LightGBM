/*!
 * Copyright (c) 2017 Microsoft Corporation. All rights reserved.
 * Licensed under the MIT License. See LICENSE file in the project root for license information.
 */
#ifndef LIGHTGBM_FEATURE_GROUP_H_
#define LIGHTGBM_FEATURE_GROUP_H_

#include <cstdio>
#include <memory>
#include <vector>

#include <LightGBM/bin.h>
#include <LightGBM/meta.h>
#include <LightGBM/utils/random.h>

namespace LightGBM {

class Dataset;
class DatasetLoader;
/*! \brief Using to store data and providing some operations on one feature group*/
class FeatureGroup {
 public:
  friend Dataset;
  friend DatasetLoader;
  /*!
  * \brief Constructor
  * \param num_feature number of features of this group
  * \param bin_mappers Bin mapper for features
  * \param num_data Total number of data
  * \param is_enable_sparse True if enable sparse feature
  */
  FeatureGroup(int num_feature, bool is_multi_val,
    std::vector<std::unique_ptr<BinMapper>>* bin_mappers,
    data_size_t num_data) : num_feature_(num_feature), is_multi_val_(is_multi_val), is_sparse_(false) {
    CHECK_EQ(static_cast<int>(bin_mappers->size()), num_feature);
    // use bin at zero to store most_freq_bin
    num_total_bin_ = 1;
    bin_offsets_.emplace_back(num_total_bin_);
    auto& ref_bin_mappers = *bin_mappers;
    for (int i = 0; i < num_feature_; ++i) {
      bin_mappers_.emplace_back(ref_bin_mappers[i].release());
      auto num_bin = bin_mappers_[i]->num_bin();
      if (bin_mappers_[i]->GetMostFreqBin() == 0) {
        num_bin -= 1;
      }
      num_total_bin_ += num_bin;
      bin_offsets_.emplace_back(num_total_bin_);
    }
    CreateBinData(num_data, is_multi_val_, true, false);
  }

  FeatureGroup(const FeatureGroup& other, int num_data) {
    num_feature_ = other.num_feature_;
    is_multi_val_ = other.is_multi_val_;
    is_sparse_ = other.is_sparse_;
    num_total_bin_ = other.num_total_bin_;
    bin_offsets_ = other.bin_offsets_;

    bin_mappers_.reserve(other.bin_mappers_.size());
    for (auto& bin_mapper : other.bin_mappers_) {
      bin_mappers_.emplace_back(new BinMapper(*bin_mapper));
    }
    CreateBinData(num_data, is_multi_val_, !is_sparse_, is_sparse_);
  }

  FeatureGroup(std::vector<std::unique_ptr<BinMapper>>* bin_mappers,
    data_size_t num_data) : num_feature_(1), is_multi_val_(false) {
    CHECK_EQ(static_cast<int>(bin_mappers->size()), 1);
    // use bin at zero to store default_bin
    num_total_bin_ = 1;
    bin_offsets_.emplace_back(num_total_bin_);
    auto& ref_bin_mappers = *bin_mappers;
    for (int i = 0; i < num_feature_; ++i) {
      bin_mappers_.emplace_back(ref_bin_mappers[i].release());
      auto num_bin = bin_mappers_[i]->num_bin();
      if (bin_mappers_[i]->GetMostFreqBin() == 0) {
        num_bin -= 1;
      }
      num_total_bin_ += num_bin;
      bin_offsets_.emplace_back(num_total_bin_);
    }
    CreateBinData(num_data, false, false, false);
  }

  /*!
  * \brief Constructor from memory
  * \param memory Pointer of memory
  * \param num_all_data Number of global data
  * \param local_used_indices Local used indices, empty means using all data
  */
  FeatureGroup(const void* memory, data_size_t num_all_data,
    const std::vector<data_size_t>& local_used_indices) {
    const char* memory_ptr = reinterpret_cast<const char*>(memory);
    // get is_sparse
    is_multi_val_ = *(reinterpret_cast<const bool*>(memory_ptr));
    memory_ptr += sizeof(is_multi_val_);
    is_sparse_ = *(reinterpret_cast<const bool*>(memory_ptr));
    memory_ptr += sizeof(is_sparse_);
    num_feature_ = *(reinterpret_cast<const int*>(memory_ptr));
    memory_ptr += sizeof(num_feature_);
    // get bin mapper
    bin_mappers_.clear();
    bin_offsets_.clear();
    // start from 1, due to need to store zero bin in this slot
    num_total_bin_ = 1;
    bin_offsets_.emplace_back(num_total_bin_);
    for (int i = 0; i < num_feature_; ++i) {
      bin_mappers_.emplace_back(new BinMapper(memory_ptr));
      auto num_bin = bin_mappers_[i]->num_bin();
      if (bin_mappers_[i]->GetMostFreqBin() == 0) {
        num_bin -= 1;
      }
      num_total_bin_ += num_bin;
      bin_offsets_.emplace_back(num_total_bin_);
      memory_ptr += bin_mappers_[i]->SizesInByte();
    }
    data_size_t num_data = num_all_data;
    if (!local_used_indices.empty()) {
      num_data = static_cast<data_size_t>(local_used_indices.size());
    }
    if (is_multi_val_) {
      for (int i = 0; i < num_feature_; ++i) {
        int addi = bin_mappers_[i]->GetMostFreqBin() == 0 ? 0 : 1;
        if (bin_mappers_[i]->sparse_rate() >= kSparseThreshold) {
          multi_bin_data_.emplace_back(Bin::CreateSparseBin(num_data, bin_mappers_[i]->num_bin() + addi));
        } else {
          multi_bin_data_.emplace_back(Bin::CreateDenseBin(num_data, bin_mappers_[i]->num_bin() + addi));
        }
        multi_bin_data_.back()->LoadFromMemory(memory_ptr, local_used_indices);
        memory_ptr += multi_bin_data_.back()->SizesInByte();
      }
    } else {
      if (is_sparse_) {
        bin_data_.reset(Bin::CreateSparseBin(num_data, num_total_bin_));
      } else {
        bin_data_.reset(Bin::CreateDenseBin(num_data, num_total_bin_));
      }
      // get bin data
      bin_data_->LoadFromMemory(memory_ptr, local_used_indices);
    }
  }

  /*! \brief Destructor */
  ~FeatureGroup() {
  }

  /*!
  * \brief Push one record, will auto convert to bin and push to bin data
  * \param tid Thread id
  * \param idx Index of record
  * \param value feature value of record
  */
  inline void PushData(int tid, int sub_feature_idx, data_size_t line_idx, double value) {
    uint32_t bin = bin_mappers_[sub_feature_idx]->ValueToBin(value);
    if (bin == bin_mappers_[sub_feature_idx]->GetMostFreqBin()) { return; }
    if (bin_mappers_[sub_feature_idx]->GetMostFreqBin() == 0) {
      bin -= 1;
    }
    if (is_multi_val_) {
      multi_bin_data_[sub_feature_idx]->Push(tid, line_idx, bin + 1);
    } else {
      bin += bin_offsets_[sub_feature_idx];
      bin_data_->Push(tid, line_idx, bin);
    }
  }

  void ReSize(int num_data) {
    if (!is_multi_val_) {
      bin_data_->ReSize(num_data);
    } else {
      for (int i = 0; i < num_feature_; ++i) {
        multi_bin_data_[i]->ReSize(num_data);
      }
    }
  }

  inline void CopySubrow(const FeatureGroup* full_feature, const data_size_t* used_indices, data_size_t num_used_indices) {
    if (!is_multi_val_) {
      bin_data_->CopySubrow(full_feature->bin_data_.get(), used_indices, num_used_indices);
    } else {
      for (int i = 0; i < num_feature_; ++i) {
        multi_bin_data_[i]->CopySubrow(full_feature->multi_bin_data_[i].get(), used_indices, num_used_indices);
      }
    }
  }

  inline BinIterator* SubFeatureIterator(int sub_feature) {
    uint32_t most_freq_bin = bin_mappers_[sub_feature]->GetMostFreqBin();
    if (!is_multi_val_) {
      uint32_t min_bin = bin_offsets_[sub_feature];
      uint32_t max_bin = bin_offsets_[sub_feature + 1] - 1;
      return bin_data_->GetIterator(min_bin, max_bin, most_freq_bin);
    } else {
      int addi = bin_mappers_[sub_feature]->GetMostFreqBin() == 0 ? 0 : 1;
      uint32_t min_bin = 1;
      uint32_t max_bin = bin_mappers_[sub_feature]->num_bin() - 1 + addi;
      return multi_bin_data_[sub_feature]->GetIterator(min_bin, max_bin, most_freq_bin);
    }
  }

  inline void FinishLoad() {
    if (is_multi_val_) {
      OMP_INIT_EX();
      #pragma omp parallel for schedule(guided)
      for (int i = 0; i < num_feature_; ++i) {
        OMP_LOOP_EX_BEGIN();
        multi_bin_data_[i]->FinishLoad();
        OMP_LOOP_EX_END();
      }
      OMP_THROW_EX();
    } else {
      bin_data_->FinishLoad();
    }
  }

  /*!
   * \brief Returns a BinIterator that can access the entire feature group's raw data.
   *        The RawGet() function of the iterator should be called for best efficiency.
   * \return A pointer to the BinIterator object
   */
  inline BinIterator* FeatureGroupIterator() {
    if (is_multi_val_) {
      return nullptr;
    }
    uint32_t min_bin = bin_offsets_[0];
    uint32_t max_bin = bin_offsets_.back() - 1;
    uint32_t most_freq_bin = 0;
    return bin_data_->GetIterator(min_bin, max_bin, most_freq_bin);
  }

  inline data_size_t Split(int sub_feature, const uint32_t* threshold,
                           int num_threshold, bool default_left,
                           const data_size_t* data_indices, data_size_t cnt,
                           data_size_t* lte_indices,
                           data_size_t* gt_indices) const {
    uint32_t default_bin = bin_mappers_[sub_feature]->GetDefaultBin();
    uint32_t most_freq_bin = bin_mappers_[sub_feature]->GetMostFreqBin();
    if (!is_multi_val_) {
      uint32_t min_bin = bin_offsets_[sub_feature];
      uint32_t max_bin = bin_offsets_[sub_feature + 1] - 1;
      if (bin_mappers_[sub_feature]->bin_type() == BinType::NumericalBin) {
        auto missing_type = bin_mappers_[sub_feature]->missing_type();
        if (num_feature_ == 1) {
          return bin_data_->Split(max_bin, default_bin, most_freq_bin,
                                  missing_type, default_left, *threshold,
                                  data_indices, cnt, lte_indices, gt_indices);
        } else {
          return bin_data_->Split(min_bin, max_bin, default_bin, most_freq_bin,
                                  missing_type, default_left, *threshold,
                                  data_indices, cnt, lte_indices, gt_indices);
        }
      } else {
        if (num_feature_ == 1) {
          return bin_data_->SplitCategorical(max_bin, most_freq_bin, threshold,
                                             num_threshold, data_indices, cnt,
                                             lte_indices, gt_indices);
        } else {
          return bin_data_->SplitCategorical(
              min_bin, max_bin, most_freq_bin, threshold, num_threshold,
              data_indices, cnt, lte_indices, gt_indices);
        }
      }
    } else {
      int addi = bin_mappers_[sub_feature]->GetMostFreqBin() == 0 ? 0 : 1;
      uint32_t max_bin = bin_mappers_[sub_feature]->num_bin() - 1 + addi;
      if (bin_mappers_[sub_feature]->bin_type() == BinType::NumericalBin) {
        auto missing_type = bin_mappers_[sub_feature]->missing_type();
        return multi_bin_data_[sub_feature]->Split(
            max_bin, default_bin, most_freq_bin, missing_type, default_left,
            *threshold, data_indices, cnt, lte_indices, gt_indices);
      } else {
        return multi_bin_data_[sub_feature]->SplitCategorical(
            max_bin, most_freq_bin, threshold, num_threshold, data_indices, cnt,
            lte_indices, gt_indices);
      }
    }
  }

  /*!
  * \brief From bin to feature value
  * \param bin
  * \return FeatureGroup value of this bin
  */
  inline double BinToValue(int sub_feature_idx, uint32_t bin) const {
    return bin_mappers_[sub_feature_idx]->BinToValue(bin);
  }

  /*!
  * \brief Save binary data to file
  * \param file File want to write
  */
  void SaveBinaryToFile(const VirtualFileWriter* writer) const {
    writer->Write(&is_multi_val_, sizeof(is_multi_val_));
    writer->Write(&is_sparse_, sizeof(is_sparse_));
    writer->Write(&num_feature_, sizeof(num_feature_));
    for (int i = 0; i < num_feature_; ++i) {
      bin_mappers_[i]->SaveBinaryToFile(writer);
    }
    if (is_multi_val_) {
      for (int i = 0; i < num_feature_; ++i) {
        multi_bin_data_[i]->SaveBinaryToFile(writer);
      }
    } else {
      bin_data_->SaveBinaryToFile(writer);
    }
  }

  /*!
  * \brief Get sizes in byte of this object
  */
  size_t SizesInByte() const {
    size_t ret = sizeof(is_multi_val_) + sizeof(is_sparse_) + sizeof(num_feature_);
    for (int i = 0; i < num_feature_; ++i) {
      ret += bin_mappers_[i]->SizesInByte();
    }
    if (!is_multi_val_) {
      ret += bin_data_->SizesInByte();
    } else {
      for (int i = 0; i < num_feature_; ++i) {
        ret += multi_bin_data_[i]->SizesInByte();
      }
    }
    return ret;
  }

  /*! \brief Disable copy */
  FeatureGroup& operator=(const FeatureGroup&) = delete;

  /*! \brief Deep copy */
  FeatureGroup(const FeatureGroup& other) {
    num_feature_ = other.num_feature_;
    is_multi_val_ = other.is_multi_val_;
    is_sparse_ = other.is_sparse_;
    num_total_bin_ = other.num_total_bin_;
    bin_offsets_ = other.bin_offsets_;

    bin_mappers_.reserve(other.bin_mappers_.size());
    for (auto& bin_mapper : other.bin_mappers_) {
      bin_mappers_.emplace_back(new BinMapper(*bin_mapper));
    }
    if (!is_multi_val_) {
      bin_data_.reset(other.bin_data_->Clone());
    } else {
      multi_bin_data_.clear();
      for (int i = 0; i < num_feature_; ++i) {
        multi_bin_data_.emplace_back(other.multi_bin_data_[i]->Clone());
      }
    }
  }

 private:
  void CreateBinData(int num_data, bool is_multi_val, bool force_dense, bool force_sparse) {
    if (is_multi_val) {
      multi_bin_data_.clear();
      for (int i = 0; i < num_feature_; ++i) {
        int addi = bin_mappers_[i]->GetMostFreqBin() == 0 ? 0 : 1;
        if (bin_mappers_[i]->sparse_rate() >= kSparseThreshold) {
          multi_bin_data_.emplace_back(Bin::CreateSparseBin(
              num_data, bin_mappers_[i]->num_bin() + addi));
        } else {
          multi_bin_data_.emplace_back(
              Bin::CreateDenseBin(num_data, bin_mappers_[i]->num_bin() + addi));
        }
      }
      is_multi_val_ = true;
    } else {
      if (force_sparse || (!force_dense && num_feature_ == 1 &&
                           bin_mappers_[0]->sparse_rate() >= kSparseThreshold)) {
        is_sparse_ = true;
        bin_data_.reset(Bin::CreateSparseBin(num_data, num_total_bin_));
      } else {
        is_sparse_ = false;
        bin_data_.reset(Bin::CreateDenseBin(num_data, num_total_bin_));
      }
      is_multi_val_ = false;
    }
  }

  /*! \brief Number of features */
  int num_feature_;
  /*! \brief Bin mapper for sub features */
  std::vector<std::unique_ptr<BinMapper>> bin_mappers_;
  /*! \brief Bin offsets for sub features */
  std::vector<uint32_t> bin_offsets_;
  /*! \brief Bin data of this feature */
  std::unique_ptr<Bin> bin_data_;
  std::vector<std::unique_ptr<Bin>> multi_bin_data_;
  /*! \brief True if this feature is sparse */
  bool is_multi_val_;
  bool is_sparse_;
  int num_total_bin_;
};


}  // namespace LightGBM

#endif   // LIGHTGBM_FEATURE_GROUP_H_
