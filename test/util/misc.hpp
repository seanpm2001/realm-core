/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
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

#ifndef REALM_TEST_UTIL_MISC_HPP
#define REALM_TEST_UTIL_MISC_HPP

#include <string>

namespace realm {
namespace test_util {

void replace_all(std::string& str, const std::string& from, const std::string& to);
bool equal_without_cr(std::string s1, std::string s2);

struct ForkedProcess {
    ForkedProcess(const std::string& test_name, const std::string& ident);
    bool is_child();
    bool is_parent();
    int wait_for_child_to_finish();
    void set_pid(int id);

private:
    std::string m_test_name;
    std::string m_identifier;
#ifndef _WIN32
    int m_pid = -1;
#endif // _WIN32
};

ForkedProcess fork_and_update_mappings(const std::string& test_name, const std::string& process_ident);
int64_t get_pid();

} // namespace test_util
} // namespace realm

#endif // REALM_TEST_UTIL_MISC_HPP
