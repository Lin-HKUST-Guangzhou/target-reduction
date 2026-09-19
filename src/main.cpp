#include <vector>
#include <string>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <memory>
#include <CLI/CLI.hpp>
#include <cli/cli.h>
#include <cli/clilocalsession.h>
#include <cli/detail/split.h>
#include <cli/loopscheduler.h>
#include "aig_manager.h"
#include "command_manager.h"

namespace {

void trim(std::string &text)
{
    const auto isSpace = [](unsigned char ch) { return std::isspace(ch); };
    text.erase(text.begin(), std::find_if_not(text.begin(), text.end(), isSpace));
    text.erase(std::find_if_not(text.rbegin(), text.rend(), isSpace).base(), text.end());
}

std::vector<std::string> splitCommands(const std::string &script)
{
    std::vector<std::string> commands;
    std::stringstream stream(script);
    std::string command;
    while (std::getline(stream, command, ';')) {
        commands.push_back(command);
    }
    return commands;
}

} // namespace

void launchCmd(CmdMan & cmdman, AIGMan & aigman, std::string & cmd) {
    trim(cmd);
    if (cmd.length() == 0)
        return;

    std::vector<std::string> vLiterals;
    cli::detail::split(vLiterals, cmd);
    if (vLiterals.empty())
        return;
    
    std::string & command = vLiterals[0];

    cmdman.launchCommand(aigman, command, vLiterals);
}

void runInteractive(CmdMan & cmdman, AIGMan & aigman) {
    auto rootMenu = std::make_unique<cli::Menu>("target-reduction");

    cmdman.cliMenuAddCommands(aigman, rootMenu);
    
    cli::Cli cli( std::move(rootMenu) );
    cli.ExitAction( [](auto& out){ out << "target-reduction terminated.\n"; } );

    cli::LoopScheduler scheduler;
    cli::CliLocalTerminalSession localSession(cli, scheduler, std::cout, 400);
    localSession.ExitAction(
        [&scheduler](auto& out)
        {
            out << "terminating ...\n";
            scheduler.Stop();
        }
    );

    scheduler.Run();
}

int main(int argc, char * argv[]) {
    CLI::App app;
    

    std::string script = "";
    app.add_option("-c,--script", script, "run the provided commands in script mode");

    CLI11_PARSE(app, argc, argv);

    // create managers
    CmdMan cmdman;
    AIGMan aigman;
    if (script.length() > 0) {
        std::cout << "Running in script mode: \"" << script << "\"." << std::endl;
        std::cout << "============================================" << std::endl;

        std::vector<std::string> vCommands = splitCommands(script);

        for (std::string & cmd : vCommands)
            launchCmd(cmdman, aigman, cmd);
        std::cout << "all done" << std::endl;
        exit(0);
    } else {
        // interactive mode
        std::cout << "Starting interactive mode" << std::endl;
        runInteractive(cmdman, aigman);
    }

    return 0;
}
