local value, failure = api.add(20, 22)
assert(failure == nil and value == 42)
local invalid, error = api.fail(1, 2)
assert(invalid == nil and error ~= nil)
return value
