#include "command_manager.h"

#include <CLI/CLI.hpp>

#include <cstdio>
#include <iostream>
#include <string>

namespace {

int parseCommand(CLI::App &parser, const std::vector<std::string> &tokens)
{
    std::string command;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (index != 0) {
            command += ' ';
        }
        command += tokens[index];
    }

    try {
        parser.parse(command, true);
    } catch (const CLI::CallForHelp &) {
        std::cout << parser.help();
        return 0;
    } catch (const CLI::ParseError &) {
        return 1;
    }
    return 2;
}

int readHandler(AIGMan &aigman, const std::vector<std::string> &tokens)
{
    if (tokens.size() != 2) {
        return 1;
    }
    return aigman.readFile(tokens[1].c_str()) ? 0 : 1;
}

int writeHandler(AIGMan &aigman, const std::vector<std::string> &tokens)
{
    if (tokens.size() != 2 || tokens[1].size() < 4 ||
        tokens[1].substr(tokens[1].size() - 4) != ".aig") {
        std::printf("write: expected an .aig output path.\n");
        return 1;
    }
    aigman.saveFile(tokens[1].c_str());
    return 0;
}

int printStatsHandler(AIGMan &aigman, const std::vector<std::string> &)
{
    aigman.printStats();
    return 0;
}

int targetReductionHandler(AIGMan &aigman, const std::vector<std::string> &tokens)
{
    constexpr int kDefaultRounds = 1;
    constexpr int kDefaultEarlyStopRoundLimit = 1000;

    int rounds = kDefaultRounds;
    int earlyStop = 0;
    int timeLimit = 0;
    int maxWiresPerFanin = 3;
    int maxNumFanins = 8;
    int maxCandidateWires = 2048;
    int verbose = 0;
    bool disablePreserveLevel = false;

    CLI::App parser("Run Target-Reduction node optimization");
    auto *roundsOption = parser.add_option(
        "-R,--rounds", rounds,
        "Maximum completed rounds; 0 means unlimited (default: 1)");
    auto *earlyStopOption = parser.add_option(
        "-Q,--early-stop", earlyStop,
        "Stop after Q consecutive non-improving rounds; 0 disables");
    parser.add_option("-T,--time-limit", timeLimit,
                      "Total wall-time limit in seconds; 0 disables");
    parser.add_option("-W,--wires", maxWiresPerFanin,
                      "Maximum replacement wires per target fanin (default: 3)");
    parser.add_option("-M,--max-fanins", maxNumFanins,
                      "Maximum fanins per internal MIAIG node (default: 8)");
    parser.add_option("-C,--max-candidate-wires", maxCandidateWires,
                      "Maximum retained candidate wires per target fanin (default: 2048)");
    parser.add_option("-V,--verbose", verbose,
                      "Log level: 0 summary, 1 commits, 2 failures");
    parser.add_flag("-l,--no-preserve-level", disablePreserveLevel,
                    "Disable level-preserving candidate filtering");

    const int status = parseCommand(parser, tokens);
    if (status < 2) {
        return status;
    }
    if (roundsOption->count() == 0 && earlyStopOption->count() > 0) {
        rounds = kDefaultEarlyStopRoundLimit;
    }
    if (rounds < 0 || earlyStop < 0 || timeLimit < 0 ||
        maxWiresPerFanin < 1 || maxNumFanins < 2 ||
        maxCandidateWires < 0 || verbose < 0) {
        std::printf("tred: invalid negative or out-of-range option.\n");
        return 1;
    }
    if (rounds == 0 && earlyStop == 0 && timeLimit == 0) {
        std::printf("tred: unlimited rounds require -Q or -T.\n");
        return 1;
    }

    aigman.targetReduction(rounds, earlyStop, timeLimit, maxWiresPerFanin,
                        maxNumFanins, maxCandidateWires,
                        !disablePreserveLevel, verbose);
    return 0;
}

} // namespace

void CmdMan::registerAllCommands()
{
    registerCommand("read", readHandler);
    registerCommand("write", writeHandler);
    registerCommand("ps", printStatsHandler);
    registerCommand("print_stats", printStatsHandler);
    registerCommand("tred", targetReductionHandler);
}

void CmdMan::registerCommand(const std::string &command,
                             const CommandHandler &handler)
{
    handlers.emplace(command, handler);
    commandOrder.push_back(command);
}

void CmdMan::launchCommand(AIGMan &aigman, const std::string &command,
                           const std::vector<std::string> &tokens)
{
    const auto found = handlers.find(command);
    if (found == handlers.end()) {
        std::printf("Unknown command: %s\n", command.c_str());
        return;
    }
    if (found->second(aigman, tokens) == 1) {
        std::printf("Command %s failed or has invalid arguments.\n", command.c_str());
    }
}

void CmdMan::cliMenuAddCommands(AIGMan &aigman,
                                std::unique_ptr<cli::Menu> &menu)
{
    for (const std::string &command : commandOrder) {
        menu->Insert(
            command,
            [this, &aigman, command](std::ostream &, std::vector<std::string> tokens) {
                tokens.insert(tokens.begin(), command);
                launchCommand(aigman, command, tokens);
            },
            command);
    }
}
