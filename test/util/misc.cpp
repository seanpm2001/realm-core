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

#include "misc.hpp"

#include <realm/util/assert.hpp>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <realm/util/file_mapper.hpp>

namespace realm {
namespace test_util {

void replace_all(std::string& str, const std::string& from, const std::string& to)
{
    if (from.empty())
        return;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length(); // In case 'to' contains 'from', like replacing 'x' with 'yx'
    }
}

bool equal_without_cr(std::string s1, std::string s2)
{
    // Remove CR so that we can be compare strings platform independant

    replace_all(s1, "\r", "");
    replace_all(s2, "\r", "");
    return (s1 == s2);
}

ForkedProcess::ForkedProcess(const std::string& test_name, const std::string& ident)
    : m_test_name(test_name)
    , m_identifier(ident)
{
}

void ForkedProcess::set_pid(int id)
{
#ifndef _WIN32
    m_pid = id;
#endif
}

bool ForkedProcess::is_child()
{
#ifndef _WIN32
    REALM_ASSERT_EX(m_pid >= 0, m_pid);
    return m_pid == 0;
#else

#endif
}

bool ForkedProcess::is_parent()
{
#ifndef _WIN32
    return m_pid != 0;
#else

#endif
}

int ForkedProcess::wait_for_child_to_finish()
{
#ifndef _WIN32
    int ret = 0;
    int status = 0;
    int options = 0;
    do {
        ret = waitpid(m_pid, &status, options);
    } while (ret == -1 && errno == EINTR);
    REALM_ASSERT_RELEASE_EX(ret != -1, errno, m_pid, m_test_name, m_identifier);

    bool signaled_to_stop = WIFSIGNALED(status);
    REALM_ASSERT_RELEASE_EX(!signaled_to_stop, WTERMSIG(status), WCOREDUMP(status), m_pid, m_test_name, m_identifier);

    bool stopped = WIFSTOPPED(status);
    REALM_ASSERT_RELEASE_EX(!stopped, WSTOPSIG(status), m_pid, m_test_name, m_identifier);

    bool exited_normally = WIFEXITED(status);
    REALM_ASSERT_RELEASE_EX(exited_normally, m_pid, m_test_name, m_identifier);

    auto exit_status = WEXITSTATUS(status);
    REALM_ASSERT_RELEASE_EX(exit_status == 0, exit_status, m_pid, m_test_name, m_identifier);
    return status;
#else
    constexpr bool not_supported_on_windows = false;
    REALM_ASSERT(not_supported_on_windows);
    return -1;
#endif
}

ForkedProcess fork_and_update_mappings(const std::string& test_name, const std::string& process_ident)
{
    ForkedProcess process(test_name, process_ident);
#ifndef _WIN32
    util::prepare_for_fork_in_parent();
    int pid = fork();
    REALM_ASSERT(pid >= 0);
    if (pid == 0) {
        util::post_fork_in_child();
    }
    process.set_pid(pid);
#else
    constexpr bool not_supported_on_windows = false;
    REALM_ASSERT(not_supported_on_windows);
    return -1;
#endif
    return process;
}

int64_t get_pid()
{
#ifdef _WIN32
    uint64_t pid = GetCurrentProcessId();
    return pid;
#else
    uint64_t pid = getpid();
    return pid;
#endif
}

} // namespace test_util
} // namespace realm
