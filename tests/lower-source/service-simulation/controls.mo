module Controls
expose reset

fn reset(sim: Simulation)
  Simulation.crash(sim)
  Simulation.advance(sim, 0.seconds)
  Simulation.restart(sim)
end
