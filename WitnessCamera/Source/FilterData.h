#pragma once

#include <vector>
#include <memory>

namespace Witness{
namespace Camera{

class IRecordFilter;

struct FilterDataBase
{
	std::vector<std::shared_ptr<IRecordFilter>> ChildFilters;
};

}}
