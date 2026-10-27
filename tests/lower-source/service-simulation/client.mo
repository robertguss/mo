module Client
expose history
use Ticks

fn history(sim: Simulation) : Result(List((Time, Int)), SimulationError)
  Simulation.send(sim, Ticks.history)
end
