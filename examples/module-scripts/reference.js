// Explicit module-operation resource; request/approve horo.script.reference in the real package and host policy.
const r = horo.import("horo.reference").horo.reference;
const echoed = r.echo({id:18446744073709551615n,name:"reference",payload:[0,255,65],active:true,
    ratio:0.5,names:["lua","javascript"],scores:[["same-api",1.0]],mode:r.on,token:null});
if (echoed.values[0].id !== 18446744073709551615n) throw new Error("roundtrip");
const digest = await r.digest([0,255,65]);
if (digest.error !== null) throw new Error("digest");
const rejected = r.reject("bounded semantic error");
if (rejected.error.code !== "horo.reference.rejected") throw new Error("semantic error");
return r.sum(41n);
