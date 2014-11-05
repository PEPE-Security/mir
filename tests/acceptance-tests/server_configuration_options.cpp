/*
 * Copyright © 2014 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored By: Alan Griffiths <alan@octopull.co.uk>
 */

#include "mir_test_framework/headless_test.h"

#include "mir/options/option.h"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <cstdlib>
#include <fstream>

using namespace testing;

struct ServerConfigurationOptions : mir_test_framework::HeadlessTest
{
    MOCK_METHOD1(command_line_handler, void(std::vector<std::string> const&));

    void SetUp() override
    {
        server.set_command_line_handler([this](int argc, char const* const* argv)
            {
                std::vector<std::string> args;
                for (auto p = argv; p != argv+argc; ++p)
                {
                    args.emplace_back(*p);
                }
                command_line_handler(args);
            });

        remove_config_file_in(fake_home_config);
        remove(fake_home);

        add_to_environment(env_xdg_config_home, fake_xdg_config_home);
        add_to_environment(env_home, fake_home);
        add_to_environment(env_xdg_config_dirs, fake_xdg_config_dirs);
    }

    void TearDown() override
    {
//        remove_config_file_in(fake_home_config);
//        remove(fake_home);
    }

    static constexpr char const* const env_xdg_config_home = "XDG_CONFIG_HOME";
    static constexpr char const* const env_home = "HOME";
    static constexpr char const* const env_xdg_config_dirs = "XDG_CONFIG_DIRS";

    static constexpr char const* const fake_xdg_config_home = "fake_xdg_config_home";
    static constexpr char const* const fake_home = "fake_home";
    static constexpr char const* const fake_home_config = "fake_home/.config";
    static constexpr char const* const fake_xdg_config_dirs =
        "fake_xdg_config_dir0:fake_xdg_config_dir1";

    std::string const config_filename{"test.config"};
    static constexpr char const* const test_config_key = "config_dir";

    void create_config_file_in(char const* dir)
    {
        auto const filename = dir + ('/' + config_filename);

        std::ofstream config(filename);
        config << test_config_key << '=' << dir << std::endl;
    }

    void remove_config_file_in(char const* dir)
    {
        remove((dir + ('/' + config_filename)).c_str());
        remove(dir);
    }
};

TEST_F(ServerConfigurationOptions, unknown_command_line_options_are_passed_to_handler)
{
    const int argc = 10;
    char const* argv[argc] = {
        __PRETTY_FUNCTION__,
        "--enable-input", "no",
        "--hello",
        "-f", "test_file",
        "world",
        "--offscreen",
        "--answer", "42"
    };

    server.set_command_line(argc, argv);

    EXPECT_CALL(*this, command_line_handler(
        ElementsAre(StrEq("--hello"), StrEq("world"), StrEq("--answer"), StrEq("42"))));

    server.the_session_authorizer();
}

TEST_F(ServerConfigurationOptions, are_read_from_home_config_file)
{
    add_to_environment(env_xdg_config_home, nullptr);
    ASSERT_THAT(mkdir(fake_home, 0700), Eq(0));
    ASSERT_THAT(mkdir(fake_home_config, 0700), Eq(0));
    create_config_file_in(fake_home_config);

    server.add_configuration_option(test_config_key, "", mir::OptionType::string);
    EXPECT_CALL(*this, command_line_handler(_)).Times(Exactly(0));

    server.set_config_filename(config_filename);
    server.the_session_authorizer();

    EXPECT_TRUE(server.get_options()->is_set(test_config_key));
}
