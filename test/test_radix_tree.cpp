/*************************************************************************
 *
 * Copyright 2023 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include "testsettings.hpp"
#ifdef TEST_RADIX_TREE

#include <realm.hpp>
#include <realm/index_string.hpp>
#include <realm/query_expression.hpp>
#include <realm/tokenizer.hpp>
#include <realm/util/to_string.hpp>
#include <set>
#include "test.hpp"
#include "util/misc.hpp"
#include "util/random.hpp"

using namespace realm;
using namespace util;
using namespace realm;
using namespace realm::util;
using namespace realm::test_util;
using unit_test::TestContext;

TEST(RadixTree_BuildIndex)
{
    std::vector<std::string_view> strings = {
        "John", "Brian", "Samantha", "Tom", "Johnathan", "Johnny", "Sam",
    };
    Table table;
    ColKey col_pk = table.add_column(type_Int, "pk");
    table.set_primary_key_column(col_pk);
    ColKey col_key = table.add_column(type_String, "foo");

    int64_t pk = 0;
    for (auto str : strings) {
        table.create_object_with_primary_key(pk++).set(col_key, StringData(str));
    }
    table.create_object_with_primary_key(pk++).set(col_key, StringData(strings[0])); // duplicate

    // Create a new index on column
    table.add_search_index(col_key);
    StringIndex* ndx = table.get_string_index(col_key);
    IntegerIndex* int_index = table.get_int_index(col_pk);
    CHECK(int_index);

    pk = 0;
    for (auto str : strings) {
        const ObjKey key = ndx->find_first(StringData(str));
        CHECK(key);
        CHECK_EQUAL(table.get_primary_key(key), pk++);
    }
    while (table.size()) {
        table.remove_object(table.begin());
    }
}

#endif // TEST_RADIX_TREE
