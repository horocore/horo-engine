-- Explicit module-operation resource; request/approve horo.script.reference in the real package and host policy.
local r = horo.import("horo.reference").horo.reference
local echoed = r.echo({id=horo.u64("18446744073709551615"),name="reference",payload="\0\255A",active=true,
    ratio=0.5,names={"lua","javascript"},scores={{"same-api",1.0}},mode=r.on,token=horo.null})
assert(echoed.values[1].id.decimal == "18446744073709551615")
local digest = r.digest("\0\255A") -- Actual coroutine suspension/owner delivery.
assert(digest.error == horo.null)
local rejected = r.reject("bounded semantic error")
assert(rejected.error.code == "horo.reference.rejected")
return r.sum(41) -- The canonical optional second argument defaults to one.
