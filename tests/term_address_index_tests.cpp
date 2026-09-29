#include "world/term_address_index.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace swegca::world;

namespace {

template<class Function>
bool rejects(Function&& function) {
    try { function(); }
    catch (const std::invalid_argument&) { return true; }
    return false;
}

std::string numbered(const std::size_t value) {
    std::ostringstream result;
    result << "new-" << std::setw(4) << std::setfill('0') << value;
    return result.str();
}

void run_many_waves(std::vector<std::string> keys) {
    const auto original = TermAddressIndex::build({"base-a", "base-b"});
    auto current = original;
    std::unordered_map<std::string, std::size_t> expected{
        {"base-a", 0}, {"base-b", 1}};
    for (std::size_t start = 0; start < keys.size(); start += 7) {
        const auto parent = current;
        const auto count = std::min<std::size_t>(7, keys.size() - start);
        const std::span<const std::string> tail(keys.data() + start, count);
        current = parent->append(tail);
        assert(current->base_dictionary_identity() == original->base_dictionary_identity());
        for (const auto& key : tail) {
            expected.emplace(key, expected.size());
            assert(!parent->contains(key));
        }
        assert(current->size() == expected.size());
        for (const auto& [key, value] : expected)
            assert(current->lookup(key) == value);
        assert(current->tail_node_count() == expected.size() - 2);
        assert(current->tail_height() <=
               static_cast<std::size_t>(2 *
                   std::bit_width(current->tail_node_count() + 1)));
    }
    assert(original->size() == 2 && !original->contains("new-0000"));
    const std::vector<std::string> branch_key{"branch-only"};
    const auto branch = original->append(branch_key);
    assert(!current->contains("branch-only"));
    assert(!branch->contains("new-0000"));
    assert(branch->lookup("branch-only") == 2);
}

void test_persistent_avl_all_orders() {
    std::vector<std::string> ascending;
    ascending.reserve(1024);
    for (std::size_t index = 0; index < 1024; ++index)
        ascending.push_back(numbered(index));
    run_many_waves(ascending);
    auto descending = ascending;
    std::reverse(descending.begin(), descending.end());
    run_many_waves(std::move(descending));
    auto random = ascending;
    std::mt19937 engine(827);
    std::shuffle(random.begin(), random.end(), engine);
    run_many_waves(std::move(random));
}

void test_source_binding_duplicates_and_immutability() {
    const auto index = TermAddressIndex::build({"one", "two"});
    const std::vector<std::string> three{"three"};
    const auto successor = index->append(three);
    const std::vector<std::string> none;
    assert(successor->append(none).get() == successor.get());
    successor->require_source(successor->terms());
    const auto equal_but_distinct = TermAddressIndex::build(
        successor->terms()->materialize());
    assert(rejects([&] { successor->require_source(equal_but_distinct->terms()); }));

    for (const auto& addresses : std::vector<std::vector<std::string>>{
             {"one"}, {"three"}, {"four", "four"}})
        assert(rejects([&] { (void)successor->append(addresses); }));
    assert(successor->size() == 3);
    assert(successor->term(0) == "one" && successor->term(1) == "two" &&
           successor->term(2) == "three");
    assert(rejects([] { (void)TermAddressIndex::build({"duplicate", "duplicate"}); }));
    bool outside = false;
    try { (void)successor->term(3); }
    catch (const std::out_of_range&) { outside = true; }
    assert(outside);
}

void test_shared_ordered_tail_keeps_one_cold_base() {
    std::vector<std::string> original_terms;
    original_terms.reserve(10000);
    for (std::size_t index = 0; index < 10000; ++index)
        original_terms.push_back("base:" + std::to_string(index));
    const auto original = TermAddressIndex::build(std::move(original_terms));
    const auto cold_base = original->terms()->cold_base();
    auto current = original;
    std::vector<std::shared_ptr<const TermAddressIndex>> versions;
    versions.reserve(100);
    for (std::size_t index = 0; index < 100; ++index) {
        const std::vector<std::string> one{"new:" + std::to_string(index)};
        current = current->append_shared(one);
        versions.push_back(current);
        assert(current->terms()->cold_base() == cold_base);
        assert(current->terms()->shared_tail());
    }
    assert(current->size() == 10100);
    assert(current->term(9999) == "base:9999");
    assert(current->term(10000) == "new:0");
    assert(current->term(10001) == "new:1");
    assert(current->term(10099) == "new:99");
    for (std::size_t index = 0; index < versions.size(); ++index) {
        assert(versions[index]->lookup("new:" + std::to_string(index)) == 10000 + index);
        assert(versions[index]->term(versions[index]->size() - 1) ==
               "new:" + std::to_string(index));
    }
    const std::vector<std::string> duplicate{"new:0"};
    assert(rejects([&] { (void)current->append_shared(duplicate); }));

    const std::vector<std::string> last{"last"};
    const auto legacy = current->append(last);
    assert(!legacy->terms()->shared_tail());
    assert(legacy->term(legacy->size() - 1) == "last");
    assert(legacy->lookup("new:99") == 10099);
    assert(legacy->base_dictionary_identity() == original->base_dictionary_identity());
}

}  // namespace

int main() {
    test_persistent_avl_all_orders();
    test_source_binding_duplicates_and_immutability();
    test_shared_ordered_tail_keeps_one_cold_base();
    assert(term_address_index_source_sha256 ==
           "c65c30b6b168ddfef17a9ae747cdb5636a053c5f7fb5052ac92f7cc5dc42d1e9");
    std::cout << "term address index tests passed\n";
}
