module Main

fn helper(sim: Simulation)
  Simulation.send(sim, Main.helper)
end
