/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

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

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "search.h"
#include "timeman.h"
#include "uci.h"

TimeManagement Time; // Our global time management object

namespace {

  enum TimeType { OptimumTime, MaxTime };

  double gauss(int x, double a, double b)
  {
      return exp(-(x - a) * (x - a) / b);
  }

  template<TimeType T>
  int remaining(int myTime, int myInc, int moveOverhead, int movesToGo, int ply, Value eval, Value npm)
  {
    double TRatio, sd = 8.5;
    Value inpm = 4 * (KnightValueMg + BishopValueMg + RookValueMg) + 2 * QueenValueMg; // initial non_pawn_material
    npm = std::min(npm, inpm);

    int hply = (ply + 1) / 2; // current move number for any side
    int thply = int(hply + 0.0 * sqrt(abs(eval)) + 0.0 * sqrt(log(double(inpm) / npm))); // theoretical move number used for the purpose of time management

    /// In movestogo case we distribute time according to normal distribution with the maximum around move 17.
 
    if (movesToGo)
        TRatio = (T == OptimumTime ? 0.9588 : 6.044) * gauss(movesToGo, 23.0, 1900.0) / movesToGo;
    else
    {
        /// In sudden death case we increase usage of remaining time as the game goes on. This is controlled by parameter sd.

        sd = 1.0 + 15.0 * thply / (500.0 + thply);
        TRatio = (T == OptimumTime ? 0.016 : 0.085) * sd;
    }
    
    /// We distribute usage of increment according to normal distribution with the maximum around theoretical move 46.

    double ratio = std::min(1.0, TRatio * (1.0 + (44.8 + 54.3 * gauss(thply, 46.3, 428.5)) * myInc / (myTime * sd)));
    int hypMyTime = std::max(0, myTime - moveOverhead);

    return int(hypMyTime * ratio); // Intel C++ asks for an explicit cast
  }

} // namespace


/// init() is called at the beginning of the search and calculates the allowed
/// thinking time out of the time control and current game ply. We support four
/// different kinds of time controls, passed in 'limits':
///
///  inc == 0 && movestogo == 0 means: x basetime  [sudden death!]
///  inc == 0 && movestogo != 0 means: x moves in y minutes
///  inc >  0 && movestogo == 0 means: x basetime + z increment
///  inc >  0 && movestogo != 0 means: x moves in y minutes + z increment

void TimeManagement::init(Search::LimitsType& limits, Color us, int ply, Value eval, Value npm)
{
  int moveOverhead    = Options["Move Overhead"];
  int npmsec          = Options["nodestime"];

  // If we have to play in 'nodes as time' mode, then convert from time
  // to nodes, and use resulting values in time management formulas.
  // WARNING: Given npms (nodes per millisecond) must be much lower then
  // the real engine speed to avoid time losses.
  if (npmsec)
  {
      if (!availableNodes) // Only once at game start
          availableNodes = npmsec * limits.time[us]; // Time is in msec

      // Convert from millisecs to nodes
      limits.time[us] = (int)availableNodes;
      limits.inc[us] *= npmsec;
      limits.npmsec = npmsec;
  }

  startTime = limits.startTime;

      optimumTime = remaining<OptimumTime>(limits.time[us], limits.inc[us], moveOverhead, limits.movestogo, ply, eval, npm);
      maximumTime = remaining<MaxTime    >(limits.time[us], limits.inc[us], moveOverhead, limits.movestogo, ply, eval, npm);

  if (Options["Ponder"])
      optimumTime += optimumTime / 4;
}
