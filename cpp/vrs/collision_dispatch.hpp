#pragma once
#include "swegca_architecture/association_kernel.hpp"
#include <span>
#include <memory>
#include <string>
#include <functional>
namespace swegca::vrs {
// Workers pull ranges from one queue, immediately after their previous range.
// Thread count never bounds the number of outstanding logical core operations.
class CollisionDispatch {
public:
 using Evidence=architecture::kernel::AssociationEvidence;
 using Judgment=architecture::kernel::AssociationJudgment;
 explicit CollisionDispatch(const std::string& progress_directory);
 ~CollisionDispatch();
 using Commit=std::function<void(std::size_t,std::size_t)>;
 void judge(std::span<const Evidence>,std::span<Judgment>,Commit commit={});
private:
 struct Impl;std::unique_ptr<Impl> impl_;
};
}
