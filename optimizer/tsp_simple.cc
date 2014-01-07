#include <iostream>

#include "base/commandlineflags.h"
#include "constraint_solver/routing.h"
#include "base/join.h"
#include "base/timer.h"
#include <base/callback.h>

#include "tsptw_data_dt.h"
#include "tsptw_solution_dt.h"
#include "routing_common/routing_common_flags.h"

DEFINE_int64(time_limit_in_ms, 2000, "Time limit in ms, 0 means no limit.");

namespace operations_research {

void TSPTWSolver(const TSPTWDataDT & data) {

  const int size = data.Size();

  RoutingModel routing(size, 1);
  routing.SetDepot(data.Depot());
  routing.SetCost(NewPermanentCallback(&data, &TSPTWDataDT::Distance));

  const int64 horizon = data.Horizon();
  routing.AddDimension(NewPermanentCallback(&data, &TSPTWDataDT::TimePlusServiceTime),
    horizon, horizon, true, "time");

  //  Setting time windows
  for (RoutingModel::NodeIndex i(1); i < size; ++i) {
    int64 index = routing.NodeToIndex(i);
    IntVar* const cumul_var = routing.CumulVar(index, "time");
    int64 const ready = data.ReadyTime(i);
    int64 const due = data.DueTime(i);

    if (ready <= 0 && due <= 0) {
      std::vector<RoutingModel::NodeIndex> *vect = new std::vector<RoutingModel::NodeIndex>(1);
      (*vect)[0] = i;
      routing.AddDisjunction(*vect, 0); // skip node for free
        cumul_var->SetMin(0);
        cumul_var->SetMax(0);
    } else if (ready > 0 || due > 0) {
      if (ready > 0) {
        cumul_var->SetMin(ready);
      }
      if (due > 0 && due < 2147483647) {
        cumul_var->SetMax(due);
      }

      std::vector<RoutingModel::NodeIndex> *vect = new std::vector<RoutingModel::NodeIndex>(1);
      (*vect)[0] = i;
      routing.AddDisjunction(*vect, 100000);
    } else {
      std::vector<RoutingModel::NodeIndex> *vect = new std::vector<RoutingModel::NodeIndex>(1);
      (*vect)[0] = i;
      routing.AddDisjunction(*vect);
    }
  }

  //  Search strategy
  // routing.set_first_solution_strategy(RoutingModel::ROUTING_DEFAULT_STRATEGY);
  // routing.set_first_solution_strategy(RoutingModel::RoutingStrategy::ROUTING_GLOBAL_CHEAPEST_ARC);
  // routing.set_first_solution_strategy(RoutingModel::RoutingStrategy::ROUTING_LOCAL_CHEAPEST_ARC);
  // routing.set_first_solution_strategy(RoutingModel::RoutingStrategy::ROUTING_PATH_CHEAPEST_ARC);
  // routing.set_first_solution_strategy(RoutingModel::RoutingStrategy::ROUTING_EVALUATOR_STRATEGY);
  // routing.set_first_solution_strategy(RoutingModel::RoutingStrategy::ROUTING_ALL_UNPERFORMED);
  // routing.set_first_solution_strategy(RoutingModel::RoutingStrategy::ROUTING_BEST_INSERTION);

  // routing.set_metaheuristic(RoutingModel::RoutingMetaheuristic::ROUTING_GREEDY_DESCENT);
  routing.set_metaheuristic(RoutingModel::RoutingMetaheuristic::ROUTING_GUIDED_LOCAL_SEARCH);
  // routing.set_metaheuristic(RoutingModel::RoutingMetaheuristic::ROUTING_SIMULATED_ANNEALING);
  // routing.set_metaheuristic(RoutingModel::RoutingMetaheuristic::ROUTING_TABU_SEARCH);

  // routing.SetCommandLineOption("routing_no_lns", "true");

  if (FLAGS_time_limit_in_ms > 0) {
    routing.UpdateTimeLimit(FLAGS_time_limit_in_ms);
  }

  Solver *solver = routing.solver();
  IntVar *costVar = routing.CostVar();

  const Assignment* solution = routing.Solve(NULL);

  if (solution != NULL) {
    std::cout << "Cost: " << solution->ObjectiveValue() << std::endl;
    TSPTWSolution sol(data, &routing, solution);
    for(int route_nbr = 0; route_nbr < routing.vehicles(); route_nbr++) {
      for (int64 index = routing.Start(route_nbr); !routing.IsEnd(index); index = solution->Value(routing.NextVar(index))) {
        std::cout << routing.IndexToNode(index) << " ";
      }
      std::cout << routing.IndexToNode(routing.Start(route_nbr)) << std::endl;
    }
  } else {
    std::cout << "No solution found..." << std::endl;
  }
}

} // namespace operations_research

int main(int argc, char **argv) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  operations_research::TSPTWDataDT tsptw_data(FLAGS_instance_file);
  operations_research::TSPTWSolver(tsptw_data);

  return 0;
}
