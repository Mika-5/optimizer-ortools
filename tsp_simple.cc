// Copyright © Mapotempo, 2013-2015
//
// This file is part of Mapotempo.
//
// Mapotempo is free software. You can redistribute it and/or
// modify since you respect the terms of the GNU Affero General
// Public License as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version.
//
// Mapotempo is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE.  See the Licenses for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with Mapotempo. If not, see:
// <http://www.gnu.org/licenses/agpl.html>
//
#include <iostream>

#include "base/commandlineflags.h"
#include "constraint_solver/routing.h"
#include "base/join.h"
#include "base/timer.h"
#include <base/callback.h>

#include "tsptw_data_dt.h"
#include "limits.h"

#include "constraint_solver/routing.h"
#include "constraint_solver/routing_flags.h"

DEFINE_int64(time_limit_in_ms, 0, "Time limit in ms, no option means no limit.");
DEFINE_int64(no_solution_improvement_limit, -1,"Iterations whitout improvement");
DEFINE_int64(initial_time_out_no_solution_improvement, 30000, "Initial time whitout improvement in ms");
DEFINE_int64(time_out_multiplier, 2, "Multiplier for the nexts time out");
DEFINE_bool(nearby, false, "Short segment priority");
DEFINE_bool(debug, false, "debug display");


namespace operations_research {

void TWBuilder(const TSPTWDataDT &data, RoutingModel &routing, Solver *solver, int64 size, int64 min_start, bool loop_route, bool unique_configuration) {
  const int size_vehicles = data.Vehicles().size();
  int64 disjunction_cost = std::min((int64)std::pow(2, 52), ((2 * data.MaxTime() + data.MaxServiceTime()) * data.MaxTimeCost() + 2 * data.MaxDistance() * data.MaxDistanceCost()) * (data.SizeMatrix() * data.SizeMatrix() + data.TWsCounter() * data.TWsCounter()) * (data.SizeRest() > 0 ? 50000: 1));
  disjunction_cost = std::pow(2, 52);
  for (RoutingModel::NodeIndex i(0); i < size; ++i) {
    int64 const first_ready = data.ReadyTime(i);
    int64 const first_due = data.DueTime(i);
    int64 const late_multiplier = data.LateMultiplier(i);
    std::vector<int64> sticky_vehicle = data.VehicleIndices(i);
    int64 index = routing.NodeToIndex(i);
    RoutingModel::NodeIndex j = i + 1;

    if (first_ready > -MAX_INT || first_due < MAX_INT) {
      if (FLAGS_debug) {
        std::cout << "Node " << i << " index " << index << " [" << (first_ready - min_start)/100 << " : " << (first_due - min_start)/100 << "]:" << data.ServiceTime(i) << std::endl;
      }
      IntVar *const cumul_var = routing.CumulVar(index, "time");

      if (first_ready > -MAX_INT) {
        cumul_var->SetMin(first_ready);
      }
      if (first_due < MAX_INT) {
        if (late_multiplier > 0) {
          routing.SetCumulVarSoftUpperBound(i, "time", first_due, late_multiplier);
        } else {
          cumul_var->SetMax(first_due);
        }
      }
    }
    if (sticky_vehicle.size() > 0) {
      for (int v = 0; v < size_vehicles; ++v) {
        bool sticked = false;
        for (int64 sticky : sticky_vehicle) {
          if (v == sticky)
            sticked = true;
        }
        if (!sticked) {
          routing.VehicleVar(index)->RemoveValue(v);
          if (data.MatrixIndex(i) == data.MatrixIndex(j)) {
            routing.VehicleVar(routing.NodeToIndex(j))->RemoveValue(v);
          }
        }
      }
    }

    std::vector<RoutingModel::NodeIndex> *vect = new std::vector<RoutingModel::NodeIndex>(1);
    (*vect)[0] = i;
    if (data.MatrixIndex(i) == data.MatrixIndex(j)) {
      int64 second_index = routing.NodeToIndex(j);
      int64 const second_ready = data.ReadyTime(j);
      int64 const second_due = data.DueTime(j);
      IntVar *const second_cumul_var = routing.CumulVar(second_index, "time");

      if (second_ready > -MAX_INT) {
        second_cumul_var->SetMin(second_ready);
        if (second_due < MAX_INT) {
          if (late_multiplier > 0) {
            routing.SetCumulVarSoftUpperBound(j, "time", second_due, late_multiplier);
          } else {
            second_cumul_var->SetMax(second_due);
          }
        }
        vect->push_back(j);
      }
      ++i;
    }
    routing.AddDisjunction(*vect, disjunction_cost);
  }
}

vector<IntVar*> RestBuilder(const TSPTWDataDT &data, RoutingModel &routing, Solver *solver, int64 size) {
  std::vector<IntVar*> breaks;
  for (TSPTWDataDT::Rest* rest: data.Rests()) {
    int vehicle_index = rest->vehicle;
    IntVar* break_position = solver->MakeIntVar(-1, MAX_INT, "break position");
    breaks.push_back(break_position);
    std::vector<int64> values;
    if (data.Vehicles()[vehicle_index]->break_size > 0) {
      for (RoutingModel::NodeIndex i(0); i < size ; ++i) {
        int64 index;
        if ( i == size - 2) {
          index = routing.Start(vehicle_index);
        } else if ( i == size - 1){
          index = routing.End(vehicle_index);
        } else {
          index = routing.NodeToIndex(i);
        }
        values.push_back(index);

        IntVar *cumul_var = routing.CumulVar(index, "time");
        IntVar *slack_var = routing.SlackVar(index, "time");
        IntVar *const vehicle_var = routing.VehicleVar(index);
        if (i == size - 2) {
          solver->AddConstraint(solver->MakeGreaterOrEqual(cumul_var, data.Vehicles()[vehicle_index]->time_start));
        } else if (i == size -1) {
          slack_var = solver->MakeIntVar(0, MAX_INT, "end slack");
        }
        // Verify if node is affected to the right vehicle
        IntVar *const remove_index = solver->MakeConditionalExpression(solver->MakeIsDifferentCstVar(vehicle_var, vehicle_index),
          solver->MakeIntConst(index), -1)->Var();
        solver->AddConstraint(solver->MakeNonEquality(break_position, remove_index));
        // Define break_duration if the break position is equal to the current node
        IntVar *const break_duration = solver->MakeConditionalExpression(solver->MakeIsEqualCstVar(break_position, index)->Var(),
          solver->MakeIntConst(rest->rest_duration), 0)->Var();
        // Add a waiting_time before the break if its timeWindow in not already open
        IntVar *const break_wait_duration = solver->MakeConditionalExpression(solver->MakeIsEqualCstVar(break_position, index)->Var(),
        solver->MakeMax(solver->MakeDifference(rest->rest_start, solver->MakeSum(cumul_var, solver->MakeIntConst(data.ServiceTime(i)))), 0), 0)->Var();
        routing.AddVariableMinimizedByFinalizer(break_wait_duration);
        // Associate the break position accordingly to its TW
        IntVar *const upper_rest_bound = solver->MakeConditionalExpression(solver->MakeIsEqualCstVar(break_position, index)->Var(),
          solver->MakeIntConst(rest->rest_end), MAX_INT)->Var();
        solver->AddConstraint(solver->MakeGreaterOrEqual(slack_var, solver->MakeSum(break_wait_duration, break_duration)));
        solver->AddConstraint(solver->MakeLessOrEqual(solver->MakeSum(cumul_var, solver->MakeIntConst(data.ServiceTime(i))), upper_rest_bound));
      }
      rest++;
    } else {
      values.push_back(-1);
    }
    break_position->SetValues(values);
    routing.AddVariableMinimizedByFinalizer(break_position);
    routing.AddToAssignment(break_position);
  }
  return breaks;
}

void TSPTWSolver(const TSPTWDataDT &data) {
  const int size_vehicles = data.Vehicles().size();
  const int size = data.Size();
  const int size_matrix = data.SizeMatrix();
  const int size_rest = data.SizeRest();

  std::vector<std::pair<RoutingModel::NodeIndex, RoutingModel::NodeIndex>> *start_ends = new std::vector<std::pair<RoutingModel::NodeIndex, RoutingModel::NodeIndex>>(size_vehicles);
  for(int v = 0; v < size_vehicles; ++v) {
    (*start_ends)[v] = std::make_pair(data.Vehicles().at(v)->start, data.Vehicles().at(v)->stop);
  }
  RoutingModel routing(size, size_vehicles, *start_ends);

  // Dimensions
  const int64 horizon = data.Horizon();
  std::vector<ResultCallback2<long long int, IntType<operations_research::_RoutingModel_NodeIndex_tag_, int>, IntType<operations_research::_RoutingModel_NodeIndex_tag_, int> >*> time_evaluators;
  std::vector<ResultCallback2<long long int, IntType<operations_research::_RoutingModel_NodeIndex_tag_, int>, IntType<operations_research::_RoutingModel_NodeIndex_tag_, int> >*> distance_evaluators;
  std::vector<ResultCallback2<long long int, IntType<operations_research::_RoutingModel_NodeIndex_tag_, int>, IntType<operations_research::_RoutingModel_NodeIndex_tag_, int> >*> order_evaluators;
  for (TSPTWDataDT::Vehicle* vehicle: data.Vehicles()) {
    time_evaluators.push_back(NewPermanentCallback(vehicle, &TSPTWDataDT::Vehicle::TimePlusServiceTime));
    distance_evaluators.push_back(NewPermanentCallback(vehicle, &TSPTWDataDT::Vehicle::Distance));
    if (FLAGS_nearby) {
      order_evaluators.push_back(NewPermanentCallback(vehicle, &TSPTWDataDT::Vehicle::TimeOrder));
    }
  }
  routing.AddDimensionWithVehicleTransits(time_evaluators, horizon, horizon, false, "time");
  routing.AddDimensionWithVehicleTransits(distance_evaluators, 0, LLONG_MAX, true, "distance");
  if (FLAGS_nearby) {
    routing.AddDimensionWithVehicleTransits(order_evaluators, horizon, horizon, true, "order");
  }

  for (int64 i = 0; i < data.Vehicles().at(0)->capacity.size(); ++i) {
    routing.AddDimension(NewPermanentCallback(&data, &TSPTWDataDT::Quantity, NewPermanentCallback(&routing, &RoutingModel::NodeToIndex), i), 0, LLONG_MAX, true, "quantity" + std::to_string(i));
  }

  Solver *solver = routing.solver();

  int64 v = 0;
  int64 min_start = MAX_INT;
  for(TSPTWDataDT::Vehicle* vehicle: data.Vehicles()) {
    // Vehicle costs
    routing.GetMutableDimension("time")->SetSpanCostCoefficientForVehicle(vehicle->cost_time_multiplier, v);
    routing.GetMutableDimension("distance")->SetSpanCostCoefficientForVehicle(vehicle->cost_distance_multiplier, v);
    routing.SetFixedCostOfVehicle(vehicle->cost_fixed, v);
    if (FLAGS_nearby) {
      routing.GetMutableDimension("order")->SetSpanCostCoefficientForVehicle((vehicle->cost_time_multiplier + vehicle->cost_distance_multiplier)/5, v);
    }
    // Vehicle time windows
    if (vehicle->time_start > -MAX_INT) {
      int64 index = routing.Start(v);
      IntVar *const cumul_var = routing.CumulVar(index, "time");
      min_start = std::min(min_start, vehicle->time_start);
      cumul_var->SetMin(vehicle->time_start);
    }
    if (vehicle->time_end < MAX_INT) {
      int64 coef = vehicle->late_multiplier;
      if(coef > 0) {
        routing.GetMutableDimension("time")->SetEndCumulVarSoftUpperBound(v, vehicle->time_end, coef);
      } else {
        int64 index = routing.End(v);
        IntVar *const cumul_var = routing.CumulVar(index, "time");
        cumul_var->SetMax(vehicle->time_end);
      }
    }

    for (int64 i = 0; i < vehicle->capacity.size(); ++i) {
      int64 coef = vehicle->overload_multiplier[i];
      if(vehicle->capacity[i] >= 0) {
        if(coef > 0) {
          routing.GetMutableDimension("quantity" + std::to_string(i))->SetEndCumulVarSoftUpperBound(v, vehicle->capacity[i], coef);
        } else {
          int64 index = routing.End(v);
          IntVar *const cumul_var = routing.CumulVar(index, "quantity" + std::to_string(i));
          cumul_var->SetMax(vehicle->capacity[i]);
        }
      }
    }
    ++v;
  }

  // Setting solve parameters indicators
  int route_nbr = 0;
  int64 previous_distance_depot_start;
  int64 previous_distance_depot_end;
  bool loop_route = true;
  bool unique_configuration = true;
  RoutingModel::NodeIndex compareNodeIndex = routing.IndexToNode(rand() % (data.SizeMatrix() - 2));
  TSPTWDataDT::Vehicle* previous_vehicle = NULL;
  for (int route_nbr = 0; route_nbr < routing.vehicles(); route_nbr++) {
    TSPTWDataDT::Vehicle* vehicle = data.Vehicles().at(route_nbr);
    RoutingModel::NodeIndex nodeIndexStart = routing.IndexToNode(routing.Start(route_nbr));
    RoutingModel::NodeIndex nodeIndexEnd = routing.IndexToNode(routing.End(route_nbr));

    int64 distance_depot_start = std::max(vehicle->Time(nodeIndexStart, compareNodeIndex), vehicle->Distance(nodeIndexStart, compareNodeIndex));
    int64 distance_depot_end = std::max(vehicle->Time(compareNodeIndex, nodeIndexEnd), vehicle->Distance(nodeIndexStart, compareNodeIndex));
    int64 distance_start_end = std::max(vehicle->Time(nodeIndexStart, nodeIndexEnd), vehicle->Distance(nodeIndexStart, nodeIndexEnd));

    if (previous_vehicle != NULL) {
      if (previous_distance_depot_start != distance_depot_start || previous_distance_depot_end != distance_depot_end) {
        unique_configuration = false;
      }
      if (distance_start_end != 0 || distance_depot_start == 0 && distance_depot_end == 0) {
        loop_route = false;
      }
    }
    previous_distance_depot_start = distance_depot_start;
    previous_distance_depot_end = distance_depot_end;
    previous_vehicle = vehicle;
  }

  // Setting visit time windows
  TWBuilder(data, routing, solver, size - 2, min_start, loop_route, unique_configuration);
  std::vector<IntVar*> breaks;
  // Setting rest time windows
  if (size_rest > 0) {
    breaks = RestBuilder(data, routing, solver, size);
  }

  RoutingSearchParameters parameters = BuildSearchParametersFromFlags();

  // Search strategy
  // parameters.set_first_solution_strategy(FirstSolutionStrategy::FIRST_UNBOUND_MIN_VALUE); // Default
  // parameters.set_first_solution_strategy(FirstSolutionStrategy::PATH_CHEAPEST_ARC);
  // parameters.set_first_solution_strategy(FirstSolutionStrategy::PATH_MOST_CONSTRAINED_ARC);
  // parameters.set_first_solution_strategy(FirstSolutionStrategy::CHRISTOFIDES);
  if (FLAGS_debug) std::cout << "First solution strategy : ";
  if (size_rest == 0 && loop_route && unique_configuration) {
    if (FLAGS_debug) std::cout << "Savings" << std::endl;
    parameters.set_first_solution_strategy(FirstSolutionStrategy::SAVINGS);
  } else if (unique_configuration || loop_route) {
    if (FLAGS_debug) std::cout << "Best Insertion" << std::endl;
    parameters.set_first_solution_strategy(FirstSolutionStrategy::PARALLEL_CHEAPEST_INSERTION);
  } else {
    if (FLAGS_debug) std::cout << "Default" << std::endl;
  }
  // parameters.set_first_solution_strategy(FirstSolutionStrategy::ALL_UNPERFORMED);
  // parameters.set_first_solution_strategy(FirstSolutionStrategy::BEST_INSERTION);
  // parameters.set_first_solution_strategy(FirstSolutionStrategy::ROUTING_BEST_INSERTION);
  // parameters.set_first_solution_strategy(FirstSolutionStrategy::PARALLEL_CHEAPEST_INSERTION);
  // parameters.set_first_solution_strategy(FirstSolutionStrategy::LOCAL_CHEAPEST_INSERTION);
  // parameters.set_first_solution_strategy(FirstSolutionStrategy::GLOBAL_CHEAPEST_ARC);
  // parameters.set_first_solution_strategy(FirstSolutionStrategy::LOCAL_CHEAPEST_ARC);
  // parameters.set_first_solution_strategy(FirstSolutionStrategy::ROUTING_BEST_INSERTION);

  // parameters.set_local_search_metaheuristic(LocalSearchMetaheuristic::GREEDY_DESCENT);
  parameters.set_local_search_metaheuristic(LocalSearchMetaheuristic::GUIDED_LOCAL_SEARCH);
  // parameters.set_guided_local_search_lambda_coefficient(0.5);
  // parameters.set_local_search_metaheuristic(LocalSearchMetaheuristic::SIMULATED_ANNEALING);
  // parameters.set_local_search_metaheuristic(LocalSearchMetaheuristic::TABU_SEARCH);

  if (FLAGS_time_limit_in_ms > 0) {
    parameters.set_time_limit_ms(FLAGS_time_limit_in_ms);
  }

  routing.CloseModelWithParameters(parameters);

  LoggerMonitor * const logger = MakeLoggerMonitor(data, &routing, min_start, FLAGS_debug, true);
  routing.AddSearchMonitor(logger);

  if (data.Size() > 3) {
    if (FLAGS_no_solution_improvement_limit > 0) {
      NoImprovementLimit * const no_improvement_limit = MakeNoImprovementLimit(routing.solver(), routing.CostVar(), FLAGS_no_solution_improvement_limit, FLAGS_initial_time_out_no_solution_improvement, FLAGS_time_out_multiplier, true);
      routing.AddSearchMonitor(no_improvement_limit);
    }
  } else {
    SearchLimit * const limit = solver->MakeLimit(kint64max,kint64max,kint64max,1);
    routing.AddSearchMonitor(limit);
  }

  const Assignment *solution = routing.SolveWithParameters(parameters);

  if (solution != NULL) {
    float cost = solution->ObjectiveValue() / 500.0; // Back to original cost value after GetMutableDimension("time")->SetSpanCostCoefficientForAllVehicles(5)
    logger->GetFinalLog();
    int current_break = 0;
    for (int route_nbr = 0; route_nbr < routing.vehicles(); route_nbr++) {
      for (int64 index = routing.Start(route_nbr); !routing.IsEnd(index); index = solution->Value(routing.NextVar(index))) {
        RoutingModel::NodeIndex nodeIndex = routing.IndexToNode(index);
        std::cout << data.MatrixIndex(nodeIndex) << ",";
        if (current_break < data.Rests().size() && data.Vehicles().at(route_nbr)->break_size > 0 && solution->Value(breaks[current_break]) == index) {
          std::cout << size_matrix + current_break << ",";
          current_break++;
        }
      }
      if (current_break < data.Rests().size() && data.Vehicles().at(route_nbr)->break_size > 0 && solution->Value(breaks[current_break]) == routing.End(route_nbr)) {
          std::cout << size_matrix + current_break << ",";
          current_break++;
      }
      std::cout << data.MatrixIndex(routing.IndexToNode(routing.End(route_nbr))) << ";";
    }
    std::cout << std::endl;
  } else {
    std::cout << "No solution found..." << std::endl;
  }
}

} // namespace operations_research

int main(int argc, char **argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if(FLAGS_time_limit_in_ms > 0 || FLAGS_no_solution_improvement_limit > 0) {
    operations_research::TSPTWDataDT tsptw_data(FLAGS_instance_file);
    operations_research::TSPTWSolver(tsptw_data);
  } else {
    std::cout << "No Stop condition" << std::endl;
  }

  return 0;
}