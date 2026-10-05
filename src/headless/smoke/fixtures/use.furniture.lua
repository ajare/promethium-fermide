-- Lifecycle contract fixture, independent of demonstration/migration content.
local function use(agent, world, marker)
  calls = (calls or 0) + 1
  assert(calls == 1) -- each invocation is isolated, even for shared helpers
  assert(marker.furniture.id > 0 and marker.furniture.definition ~= '')
  assert(marker.usable_point ~= '' and agent.id > 0)
  assert(not pcall(function() marker.furniture.name = 'changed' end))
  world.set_pose('sitting')
  world.claim()
  world.log('use:' .. marker.name)
end
local function finish(agent, world, marker)
  assert(agent.pose == 'sitting')
  assert(marker.furniture.id > 0 and marker.usable_point ~= '')
  world.set_pose('standing')
  world.release()
  world.log('finish:' .. marker.name)
end
return {
  api_version = 1, uuid = 'a1a1a1a1-1111-4111-8111-111111111111',
  definitions = {
    { key = 'chair', label = 'Chair',
      tiles = {{x=0,y=0,imageSet='ObjectAtlas',image='chair'}},
      usablePoints = {{key='seat',label='Seat',x=0.5,blocksPathing=false}},
      use = use, finish_use = finish },
    { key = 'sofa', label = 'Sofa',
      tiles = {{x=0,y=0,imageSet='ObjectAtlas',image='chair'}, {x=1,y=0,imageSet='ObjectAtlas',image='chair'}},
      usablePoints = {{key='left',label='Left',x=0.5,blocksPathing=false}, {key='right',label='Right',x=1.5,blocksPathing=false}},
      use = use, finish_use = finish },
    { key = 'desk', label = 'Desk',
      tiles = {{x=0,y=0,imageSet='ObjectAtlas',image='chair'}},
      usablePoints = {{key='point',label='Point',x=0.5}} }
  }
}
