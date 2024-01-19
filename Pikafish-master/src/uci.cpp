/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2024 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "uci.h"

#include <stdint.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <memory>
#include <optional>
#include <sstream>
#include <vector>

#include "benchmark.h"
#include "evaluate.h"
#include "movegen.h"
#include "nnue/evaluate_nnue.h"
#include "position.h"
#include "search.h"
#include "types.h"
#include "ucioption.h"

namespace Stockfish {

constexpr auto StartFEN = "rnbakabnr/9/1c5c1/p1p1p1p1p/9/9/P1P1P1P1P/1C5C1/9/RNBAKABNR w";
constexpr int  NormalizeToPawnValue = 368;
constexpr int  MaxHashMB            = Is64Bit ? 33554432 : 2048;

UCI::UCI(int argc, char** argv) :
    cli(argc, argv) {

    evalFile = {"EvalFile", EvalFileDefaultName, "None", ""};

    options["Debug Log File"] << Option("", [](const Option& o) { start_logger(o); });

    options["Threads"] << Option(1, 1, 1024, [this](const Option&) {
        threads.set({options, threads, tt});
    });

    options["Hash"] << Option(16, 1, MaxHashMB, [this](const Option& o) {
        threads.main_thread()->wait_for_search_finished();
        tt.resize(o, options["Threads"]);
    });

    options["Clear Hash"] << Option([this](const Option&) { search_clear(); });
    options["Ponder"] << Option(false);
    options["MultiPV"] << Option(1, 1, MAX_MOVES);
    options["Move Overhead"] << Option(10, 0, 5000);
    options["nodestime"] << Option(0, 0, 10000);
    options["UCI_ShowWDL"] << Option(false);
    options["EvalFile"] << Option(EvalFileDefaultName, [this](const Option&) {
        evalFile = Eval::NNUE::load_networks(cli.binaryDirectory, options, evalFile);
    });

    threads.set({options, threads, tt});

    search_clear();  // After threads are up
}

void UCI::loop() {

    Position     pos;
    std::string  token, cmd;
    StateListPtr states(new std::deque<StateInfo>(1));
    pos.set(StartFEN, &states->back());

    for (int i = 1; i < cli.argc; ++i)
        cmd += std::string(cli.argv[i]) + " ";

    do
    {
        if (cli.argc == 1
            && !getline(std::cin, cmd))  // Wait for an input or an end-of-file (EOF) indication
            cmd = "quit";


        std::istringstream is(cmd);

        token.clear();  // Avoid a stale if getline() returns nothing or a blank line
        is >> std::skipws >> token;

        if (token == "quit" || token == "stop")
            threads.stop = true;

        // The GUI sends 'ponderhit' to tell that the user has played the expected move.
        // So, 'ponderhit' is sent if pondering was done on the same move that the user
        // has played. The search should continue, but should also switch from pondering
        // to the normal search.
        else if (token == "ponderhit")
            threads.main_manager()->ponder = false;  // Switch to the normal search

        else if (token == "uci")
            sync_cout << "id name " << engine_info(true) << "\n"
                      << options << "\nuciok" << sync_endl;

        else if (token == "setoption")
            setoption(is);
        else if (token == "go")
            go(pos, is, states);
        else if (token == "position")
            position(pos, is, states);
        else if (token == "fen" || token == "startpos")
            is.seekg(0), position(pos, is, states);
        else if (token == "ucinewgame")
            search_clear();
        else if (token == "isready")
            sync_cout << "readyok" << sync_endl;

        // Add custom non-UCI commands, mainly for debugging purposes.
        // These commands must not be used during a search!
        else if (token == "flip")
            pos.flip();
        else if (token == "bench")
            bench(pos, is, states);
        else if (token == "d")
            sync_cout << pos << sync_endl;
        else if (token == "eval")
            trace_eval(pos);
        else if (token == "compiler")
            sync_cout << compiler_info() << sync_endl;
        else if (token == "export_net")
        {
            std::optional<std::string> filename;
            std::string                f;
            if (is >> std::skipws >> f)
                filename = f;
            Eval::NNUE::save_eval(filename, evalFile);
        }
        else if (token == "--help" || token == "help" || token == "--license" || token == "license")
            sync_cout
              << "\nPikafish is a powerful xiangqi engine for playing and analyzing."
                 "\nIt is released as free software licensed under the GNU GPLv3 License."
                 "\nPikafish is normally used with a graphical user interface (GUI) and implements"
                 "\nthe Universal Chess Interface (UCI) protocol to communicate with a GUI, an API, etc."
                 "\nFor any further information, visit https://github.com/official-pikafish/Pikafish#readme"
                 "\nor read the corresponding README.md and Copying.txt files distributed along with this program.\n"
              << sync_endl;
        else if (!token.empty() && token[0] != '#')
            sync_cout << "Unknown command: '" << cmd << "'. Type help for more information."
                      << sync_endl;

    } while (token != "quit" && cli.argc == 1);  // The command-line arguments are one-shot
}

void UCI::go(Position& pos, std::istringstream& is, StateListPtr& states) {

    Search::LimitsType limits;
    std::string        token;
    bool               ponderMode = false;

    limits.startTime = now();  // The search starts as early as possible

    while (is >> token)
        if (token == "searchmoves")  // Needs to be the last command on the line
            while (is >> token)
                limits.searchmoves.push_back(to_move(pos, token));

        else if (token == "wtime")
            is >> limits.time[WHITE];
        else if (token == "btime")
            is >> limits.time[BLACK];
        else if (token == "winc")
            is >> limits.inc[WHITE];
        else if (token == "binc")
            is >> limits.inc[BLACK];
        else if (token == "movestogo")
            is >> limits.movestogo;
        else if (token == "depth")
            is >> limits.depth;
        else if (token == "nodes")
            is >> limits.nodes;
        else if (token == "movetime")
            is >> limits.movetime;
        else if (token == "mate")
            is >> limits.mate;
        else if (token == "perft")
            is >> limits.perft;
        else if (token == "infinite")
            limits.infinite = 1;
        else if (token == "ponder")
            ponderMode = true;

    // Eval::NNUE::verify(options, evalFile);

    threads.start_thinking(pos, states, limits, ponderMode);
}

void UCI::bench(Position& pos, std::istream& args, StateListPtr& states) {
    std::string token;
    uint64_t    num, nodes = 0, cnt = 1;

    std::vector<std::string> list = setup_bench(pos, args);

    num = count_if(list.begin(), list.end(),
                   [](const std::string& s) { return s.find("go ") == 0 || s.find("eval") == 0; });

    TimePoint elapsed = now();

    for (const auto& cmd : list)
    {
        std::istringstream is(cmd);
        is >> std::skipws >> token;

        if (token == "go" || token == "eval")
        {
            std::cerr << "\nPosition: " << cnt++ << '/' << num << " (" << pos.fen() << ")"
                      << std::endl;
            if (token == "go")
            {
                go(pos, is, states);
                threads.main_thread()->wait_for_search_finished();
                nodes += threads.nodes_searched();
            }
            else
                trace_eval(pos);
        }
        else if (token == "setoption")
            setoption(is);
        else if (token == "position")
            position(pos, is, states);
        else if (token == "ucinewgame")
        {
            search_clear();  // Search::clear() may take a while
            elapsed = now();
        }
    }

    elapsed = now() - elapsed + 1;  // Ensure positivity to avoid a 'divide by zero'

    dbg_print();

    std::cerr << "\n==========================="
              << "\nTotal time (ms) : " << elapsed << "\nNodes searched  : " << nodes
              << "\nNodes/second    : " << 1000 * nodes / elapsed << std::endl;
}

void UCI::trace_eval(Position& pos) {
    StateListPtr states(new std::deque<StateInfo>(1));
    Position     p;
    p.set(pos.fen(), &states->back());

    Eval::NNUE::verify(options, evalFile);

    sync_cout << "\n" << Eval::trace(p) << sync_endl;
}

void UCI::search_clear() {
    threads.main_thread()->wait_for_search_finished();

    tt.clear(options["Threads"]);
    threads.clear();
}

void UCI::setoption(std::istringstream& is) {
    threads.main_thread()->wait_for_search_finished();
    options.setoption(is);
}

void UCI::position(Position& pos, std::istringstream& is, StateListPtr& states) {
    Move        m;
    std::string token, fen;

    is >> token;

    if (token == "startpos")
    {
        fen = StartFEN;
        is >> token;  // Consume the "moves" token, if any
    }
    else if (token == "fen")
        while (is >> token && token != "moves")
            fen += token + " ";
    else
        return;

    states = StateListPtr(new std::deque<StateInfo>(1));  // Drop the old state and create a new one
    pos.set(fen, &states->back());

    // Parse the move list, if any
    while (is >> token && (m = to_move(pos, token)) != Move::none())
    {
        states->emplace_back();
        pos.do_move(m, states->back());
    }
}

int UCI::to_cp(Value v) { return 100 * v / NormalizeToPawnValue; }

std::string UCI::value(Value v) {
    assert(-VALUE_INFINITE < v && v < VALUE_INFINITE);

    std::stringstream ss;

    if (std::abs(v) < VALUE_MATE_IN_MAX_PLY)
        ss << "cp " << to_cp(v);
    else
        ss << "mate " << (v > 0 ? VALUE_MATE - v + 1 : -VALUE_MATE - v) / 2;

    return ss.str();
}

std::string UCI::square(Square s) {
    return std::string{char('a' + file_of(s)), char('0' + rank_of(s))};
}

std::string UCI::move(Move m) {
    if (m == Move::none())
        return "(none)";

    if (m == Move::null())
        return "0000";

    Square from = m.from_sq();
    Square to   = m.to_sq();

    std::string move = square(from) + square(to);

    return move;
}

std::string UCI::pv(const Search::Worker& workerThread,
                    TimePoint             elapsed,
                    uint64_t              nodesSearched,
                    int                   hashfull) {
    std::stringstream ss;
    TimePoint         time      = elapsed + 1;
    const auto&       rootMoves = workerThread.rootMoves;
    const auto&       depth     = workerThread.completedDepth;
    const auto&       pos       = workerThread.rootPos;
    size_t            pvIdx     = workerThread.pvIdx;
    size_t            multiPV = std::min(size_t(workerThread.options["MultiPV"]), rootMoves.size());

    for (size_t i = 0; i < multiPV; ++i)
    {
        bool updated = rootMoves[i].score != -VALUE_INFINITE;

        if (depth == 1 && !updated && i > 0)
            continue;

        Depth d = updated ? depth : std::max(1, depth - 1);
        Value v = updated ? rootMoves[i].uciScore : rootMoves[i].previousScore;

        if (v == -VALUE_INFINITE)
            v = VALUE_ZERO;

        if (ss.rdbuf()->in_avail())  // Not at first line
            ss << "\n";

        ss << "info"
           << " depth " << d << " seldepth " << rootMoves[i].selDepth << " multipv " << i + 1
           << " score " << value(v);

        if (workerThread.options["UCI_ShowWDL"])
            ss << wdl(v, pos.game_ply());

        if (i == pvIdx && updated)  // previous-scores are exact
            ss << (rootMoves[i].scoreLowerbound
                     ? " lowerbound"
                     : (rootMoves[i].scoreUpperbound ? " upperbound" : ""));

        ss << " nodes " << nodesSearched << " nps " << nodesSearched * 1000 / time << " hashfull "
           << hashfull << " tbhits " << 0 << " time " << time << " pv";

        for (Move m : rootMoves[i].pv)
            ss << " " << move(m);
    }

    return ss.str();
}

namespace {
// The win rate model returns the probability of winning (in per mille units) given an
// eval and a game ply. It fits the LTC fishtest statistics rather accurately.
int win_rate_model(Value v, int ply) {

    // The model only captures up to 240 plies, so limit the input and then rescale
    double m = std::min(240, ply) / 64.0;

    // The coefficients of a third-order polynomial fit is based on the fishtest data
    // for two parameters that need to transform eval to the argument of a logistic
    // function.
    constexpr double as[] = {7.42211754, -26.5119614, 46.99271939, 340.67524114};
    constexpr double bs[] = {-0.50136481, 4.9383151, -11.86324223, 89.56581513};

    // Enforce that NormalizeToPawnValue corresponds to a 50% win rate at ply 64
    static_assert(NormalizeToPawnValue == int(as[0] + as[1] + as[2] + as[3]));

    double a = (((as[0] * m + as[1]) * m + as[2]) * m) + as[3];
    double b = (((bs[0] * m + bs[1]) * m + bs[2]) * m) + bs[3];

    // Transform the eval to centipawns with limited range
    double x = std::clamp(double(v), -4000.0, 4000.0);

    // Return the win rate in per mille units, rounded to the nearest integer
    return int(0.5 + 1000 / (1 + std::exp((a - x) / b)));
}
}

std::string UCI::wdl(Value v, int ply) {
    std::stringstream ss;

    int wdl_w = win_rate_model(v, ply);
    int wdl_l = win_rate_model(-v, ply);
    int wdl_d = 1000 - wdl_w - wdl_l;
    ss << " wdl " << wdl_w << " " << wdl_d << " " << wdl_l;

    return ss.str();
}

Move UCI::to_move(const Position& pos, std::string& str) {

    for (const auto& m : MoveList<LEGAL>(pos))
        if (str == move(m))
            return m;

    return Move::none();
}

}  // namespace Stockfish
