#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace Horo::Tests {
    // Genuine authenticated DXC output from canonical light_culling.hlsl; parser evidence, not GPU execution.
    // SHA256 ed65442e42a475b45bae72e5584074c315f918d705a8f5acab95a6f2cea4b073; input source
    // 39b5ae08d4a52448e7fef2388533361c039441cbeeb380ac43f550f56e306966.
    inline constexpr std::array<std::uint32_t, 1163> LightCullingCapturedSpirv{
        0x07230203u, 0x00010600u, 0x000e0000u, 0x0000009bu, 0x00000000u, 0x00020011u, 0x00001172u, 0x00020011u, 0x00000001u, 0x0006000bu,
        0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x000c000fu, 0x00000005u,
        0x00000002u, 0x6c6c7543u, 0x6867694cu, 0x00007374u, 0x00000003u, 0x00000004u, 0x00000005u, 0x00000006u, 0x00000007u, 0x00000008u,
        0x00060010u, 0x00000002u, 0x00000011u, 0x00000040u, 0x00000001u, 0x00000001u, 0x00040010u, 0x00000002u, 0x0000116du, 0x00000020u,
        0x00030003u, 0x00000005u, 0x00000258u, 0x000b0005u, 0x00000009u, 0x65707974u, 0x7274532eu, 0x75746375u, 0x42646572u, 0x65666675u,
        0x61502e72u, 0x64656b63u, 0x6867694cu, 0x00000074u, 0x00050005u, 0x0000000au, 0x6b636150u, 0x694c6465u, 0x00746867u, 0x00070006u,
        0x0000000au, 0x00000000u, 0x69736f70u, 0x6e6f6974u, 0x676e6152u, 0x00000065u, 0x00060006u, 0x0000000au, 0x00000001u, 0x65726964u,
        0x6f697463u, 0x0000006eu, 0x00050006u, 0x0000000au, 0x00000002u, 0x646e696bu, 0x00000000u, 0x00070006u, 0x0000000au, 0x00000003u,
        0x6f6c6f63u, 0x746e4972u, 0x69736e65u, 0x00007974u, 0x00050006u, 0x0000000au, 0x00000004u, 0x656e6f63u, 0x00000073u, 0x00060006u,
        0x0000000au, 0x00000005u, 0x6e656469u, 0x79746974u, 0x00000000u, 0x00040005u, 0x00000004u, 0x6867694cu, 0x00007374u, 0x000a0005u,
        0x0000000bu, 0x65707974u, 0x7274532eu, 0x75746375u, 0x42646572u, 0x65666675u, 0x6c432e72u, 0x65747375u, 0x00000072u, 0x00040005u,
        0x0000000cu, 0x73756c43u, 0x00726574u, 0x00050006u, 0x0000000cu, 0x00000000u, 0x6e616c70u, 0x00007365u, 0x00050005u, 0x00000005u,
        0x73756c43u, 0x73726574u, 0x00000000u, 0x000a0005u, 0x0000000du, 0x65707974u, 0x5357522eu, 0x63757274u, 0x65727574u, 0x66754264u,
        0x2e726566u, 0x69753476u, 0x0000746eu, 0x00050005u, 0x00000006u, 0x626d654du, 0x68737265u, 0x00007069u, 0x000a0005u, 0x0000000eu,
        0x65707974u, 0x5357522eu, 0x63757274u, 0x65727574u, 0x66754264u, 0x2e726566u, 0x746e6975u, 0x00000000u, 0x00050005u, 0x00000007u,
        0x65666552u, 0x636e6572u, 0x00007365u, 0x00060005u, 0x0000000fu, 0x65707974u, 0x7369442eu, 0x63746170u, 0x00000068u, 0x00060006u,
        0x0000000fu, 0x00000000u, 0x6867694cu, 0x756f4374u, 0x0000746eu, 0x00070006u, 0x0000000fu, 0x00000001u, 0x73756c43u, 0x43726574u,
        0x746e756fu, 0x00000000u, 0x00090006u, 0x0000000fu, 0x00000002u, 0x65666552u, 0x636e6572u, 0x65507365u, 0x756c4372u, 0x72657473u,
        0x00000000u, 0x00060006u, 0x0000000fu, 0x00000003u, 0x65736552u, 0x64657672u, 0x00000000u, 0x00050005u, 0x00000008u, 0x70736944u,
        0x68637461u, 0x00000000u, 0x00050005u, 0x00000002u, 0x6c6c7543u, 0x6867694cu, 0x00007374u, 0x00040047u, 0x00000003u, 0x0000000bu,
        0x0000001cu, 0x00040047u, 0x00000004u, 0x00000022u, 0x00000000u, 0x00040047u, 0x00000004u, 0x00000021u, 0x00000000u, 0x00040047u,
        0x00000005u, 0x00000022u, 0x00000000u, 0x00040047u, 0x00000005u, 0x00000021u, 0x00000001u, 0x00040047u, 0x00000006u, 0x00000022u,
        0x00000000u, 0x00040047u, 0x00000006u, 0x00000021u, 0x00000002u, 0x00040047u, 0x00000007u, 0x00000022u, 0x00000000u, 0x00040047u,
        0x00000007u, 0x00000021u, 0x00000003u, 0x00040047u, 0x00000008u, 0x00000022u, 0x00000000u, 0x00040047u, 0x00000008u, 0x00000021u,
        0x00000004u, 0x00050048u, 0x0000000au, 0x00000000u, 0x00000023u, 0x00000000u, 0x00050048u, 0x0000000au, 0x00000001u, 0x00000023u,
        0x00000010u, 0x00050048u, 0x0000000au, 0x00000002u, 0x00000023u, 0x0000001cu, 0x00050048u, 0x0000000au, 0x00000003u, 0x00000023u,
        0x00000020u, 0x00050048u, 0x0000000au, 0x00000004u, 0x00000023u, 0x00000030u, 0x00050048u, 0x0000000au, 0x00000005u, 0x00000023u,
        0x00000038u, 0x00040047u, 0x00000010u, 0x00000006u, 0x00000040u, 0x00050048u, 0x00000009u, 0x00000000u, 0x00000023u, 0x00000000u,
        0x00040048u, 0x00000009u, 0x00000000u, 0x00000018u, 0x00030047u, 0x00000009u, 0x00000002u, 0x00040047u, 0x00000011u, 0x00000006u,
        0x00000010u, 0x00050048u, 0x0000000cu, 0x00000000u, 0x00000023u, 0x00000000u, 0x00040047u, 0x00000012u, 0x00000006u, 0x00000060u,
        0x00050048u, 0x0000000bu, 0x00000000u, 0x00000023u, 0x00000000u, 0x00040048u, 0x0000000bu, 0x00000000u, 0x00000018u, 0x00030047u,
        0x0000000bu, 0x00000002u, 0x00040047u, 0x00000013u, 0x00000006u, 0x00000010u, 0x00050048u, 0x0000000du, 0x00000000u, 0x00000023u,
        0x00000000u, 0x00030047u, 0x0000000du, 0x00000002u, 0x00040047u, 0x00000014u, 0x00000006u, 0x00000004u, 0x00050048u, 0x0000000eu,
        0x00000000u, 0x00000023u, 0x00000000u, 0x00030047u, 0x0000000eu, 0x00000002u, 0x00050048u, 0x0000000fu, 0x00000000u, 0x00000023u,
        0x00000000u, 0x00050048u, 0x0000000fu, 0x00000001u, 0x00000023u, 0x00000004u, 0x00050048u, 0x0000000fu, 0x00000002u, 0x00000023u,
        0x00000008u, 0x00050048u, 0x0000000fu, 0x00000003u, 0x00000023u, 0x0000000cu, 0x00030047u, 0x0000000fu, 0x00000002u, 0x00030047u,
        0x00000015u, 0x0000002au, 0x00030047u, 0x00000016u, 0x0000002au, 0x00030047u, 0x00000017u, 0x0000002au, 0x00030047u, 0x00000018u,
        0x0000002au, 0x00030047u, 0x00000019u, 0x0000002au, 0x00030047u, 0x0000001au, 0x0000002au, 0x00030047u, 0x0000001bu, 0x0000002au,
        0x00030047u, 0x0000001cu, 0x0000002au, 0x00030047u, 0x0000001du, 0x0000002au, 0x00030047u, 0x0000001eu, 0x0000002au, 0x00040015u,
        0x0000001fu, 0x00000020u, 0x00000001u, 0x0004002bu, 0x0000001fu, 0x00000020u, 0x00000000u, 0x0004002bu, 0x0000001fu, 0x00000021u,
        0x00000003u, 0x00040015u, 0x00000022u, 0x00000020u, 0x00000000u, 0x0004002bu, 0x00000022u, 0x00000023u, 0x00000000u, 0x00020014u,
        0x00000024u, 0x00030029u, 0x00000024u, 0x00000025u, 0x0004002bu, 0x0000001fu, 0x00000026u, 0x00000001u, 0x0004002bu, 0x0000001fu,
        0x00000027u, 0x00000002u, 0x0004002bu, 0x00000022u, 0x00000028u, 0x00000001u, 0x00030016u, 0x00000029u, 0x00000020u, 0x0004002bu,
        0x00000029u, 0x0000002au, 0x00000000u, 0x0003002au, 0x00000024u, 0x0000002bu, 0x0004002bu, 0x00000022u, 0x0000002cu, 0x00000006u,
        0x00040017u, 0x0000002du, 0x00000029u, 0x00000004u, 0x00040017u, 0x0000002eu, 0x00000029u, 0x00000003u, 0x00040017u, 0x0000002fu,
        0x00000029u, 0x00000002u, 0x00040017u, 0x00000030u, 0x00000022u, 0x00000002u, 0x0008001eu, 0x0000000au, 0x0000002du, 0x0000002eu,
        0x00000022u, 0x0000002du, 0x0000002fu, 0x00000030u, 0x0003001du, 0x00000010u, 0x0000000au, 0x0003001eu, 0x00000009u, 0x00000010u,
        0x00040020u, 0x00000031u, 0x0000000cu, 0x00000009u, 0x0004001cu, 0x00000011u, 0x0000002du, 0x0000002cu, 0x0003001eu, 0x0000000cu,
        0x00000011u, 0x0003001du, 0x00000012u, 0x0000000cu, 0x0003001eu, 0x0000000bu, 0x00000012u, 0x00040020u, 0x00000032u, 0x0000000cu,
        0x0000000bu, 0x00040017u, 0x00000033u, 0x00000022u, 0x00000004u, 0x0003001du, 0x00000013u, 0x00000033u, 0x0003001eu, 0x0000000du,
        0x00000013u, 0x00040020u, 0x00000034u, 0x0000000cu, 0x0000000du, 0x0003001du, 0x00000014u, 0x00000022u, 0x0003001eu, 0x0000000eu,
        0x00000014u, 0x00040020u, 0x00000035u, 0x0000000cu, 0x0000000eu, 0x0006001eu, 0x0000000fu, 0x00000022u, 0x00000022u, 0x00000022u,
        0x00000022u, 0x00040020u, 0x00000036u, 0x00000002u, 0x0000000fu, 0x00040017u, 0x00000037u, 0x00000022u, 0x00000003u, 0x00040020u,
        0x00000038u, 0x00000001u, 0x00000037u, 0x00020013u, 0x00000039u, 0x00030021u, 0x0000003au, 0x00000039u, 0x00040020u, 0x0000003bu,
        0x00000002u, 0x00000022u, 0x00040020u, 0x0000003cu, 0x0000000cu, 0x0000000au, 0x00040020u, 0x0000003du, 0x0000000cu, 0x00000022u,
        0x00040020u, 0x0000003eu, 0x0000000cu, 0x00000033u, 0x0004003bu, 0x00000031u, 0x00000004u, 0x0000000cu, 0x0004003bu, 0x00000032u,
        0x00000005u, 0x0000000cu, 0x0004003bu, 0x00000034u, 0x00000006u, 0x0000000cu, 0x0004003bu, 0x00000035u, 0x00000007u, 0x0000000cu,
        0x0004003bu, 0x00000036u, 0x00000008u, 0x00000002u, 0x0004003bu, 0x00000038u, 0x00000003u, 0x00000001u, 0x00030001u, 0x00000024u,
        0x0000003fu, 0x00040020u, 0x00000040u, 0x0000000cu, 0x0000002du, 0x0004002bu, 0x00000022u, 0x00000041u, 0x00000002u, 0x0004002bu,
        0x00000022u, 0x00000042u, 0x00000003u, 0x00040020u, 0x00000043u, 0x0000000cu, 0x00000011u, 0x00040020u, 0x00000044u, 0x0000000cu,
        0x00000029u, 0x00050036u, 0x00000039u, 0x00000002u, 0x00000000u, 0x0000003au, 0x000200f8u, 0x00000045u, 0x0004003du, 0x00000037u,
        0x00000046u, 0x00000003u, 0x000300f7u, 0x00000047u, 0x00000000u, 0x000300fbu, 0x00000023u, 0x00000048u, 0x000200f8u, 0x00000048u,
        0x00050051u, 0x00000022u, 0x00000049u, 0x00000046u, 0x00000000u, 0x00050041u, 0x0000003bu, 0x0000004au, 0x00000008u, 0x00000021u,
        0x0004003du, 0x00000022u, 0x0000004bu, 0x0000004au, 0x000500aau, 0x00000024u, 0x0000004cu, 0x0000004bu, 0x00000023u, 0x000300f7u,
        0x0000004du, 0x00000000u, 0x000400fau, 0x0000004cu, 0x0000004eu, 0x0000004du, 0x000200f8u, 0x0000004eu, 0x00050041u, 0x0000003bu,
        0x0000004fu, 0x00000008u, 0x00000026u, 0x0004003du, 0x00000022u, 0x00000050u, 0x0000004fu, 0x000500aeu, 0x00000024u, 0x00000051u,
        0x00000049u, 0x00000050u, 0x000200f9u, 0x0000004du, 0x000200f8u, 0x0000004du, 0x000700f5u, 0x00000024u, 0x00000052u, 0x00000025u,
        0x00000048u, 0x00000051u, 0x0000004eu, 0x000300f7u, 0x00000053u, 0x00000000u, 0x000400fau, 0x00000052u, 0x00000054u, 0x00000053u,
        0x000200f8u, 0x00000054u, 0x000200f9u, 0x00000047u, 0x000200f8u, 0x00000053u, 0x00050041u, 0x0000003bu, 0x00000055u, 0x00000008u,
        0x00000027u, 0x0004003du, 0x00000022u, 0x00000056u, 0x00000055u, 0x00050084u, 0x00000022u, 0x00000015u, 0x00000049u, 0x00000056u,
        0x000200f9u, 0x00000057u, 0x000200f8u, 0x00000057u, 0x000700f5u, 0x00000024u, 0x00000058u, 0x0000003fu, 0x00000053u, 0x00000059u,
        0x0000005au, 0x000700f5u, 0x00000022u, 0x0000005bu, 0x00000023u, 0x00000053u, 0x0000005cu, 0x0000005au, 0x000700f5u, 0x00000022u,
        0x0000005du, 0x00000023u, 0x00000053u, 0x0000005eu, 0x0000005au, 0x000700f5u, 0x00000022u, 0x0000005fu, 0x00000023u, 0x00000053u,
        0x00000019u, 0x0000005au, 0x00050041u, 0x0000003bu, 0x00000060u, 0x00000008u, 0x00000020u, 0x0004003du, 0x00000022u, 0x00000061u,
        0x00000060u, 0x000500b0u, 0x00000024u, 0x00000062u, 0x0000005fu, 0x00000061u, 0x000400f6u, 0x00000063u, 0x0000005au, 0x00000000u,
        0x000400fau, 0x00000062u, 0x00000064u, 0x00000063u, 0x000200f8u, 0x00000064u, 0x00060041u, 0x0000003cu, 0x00000065u, 0x00000004u,
        0x00000020u, 0x0000005fu, 0x00050041u, 0x00000040u, 0x00000066u, 0x00000065u, 0x00000023u, 0x0004003du, 0x0000002du, 0x00000067u,
        0x00000066u, 0x00050041u, 0x0000003du, 0x00000068u, 0x00000065u, 0x00000041u, 0x0004003du, 0x00000022u, 0x00000069u, 0x00000068u,
        0x00050041u, 0x00000040u, 0x0000006au, 0x00000065u, 0x00000042u, 0x0004003du, 0x0000002du, 0x0000006bu, 0x0000006au, 0x00070041u,
        0x00000043u, 0x0000006cu, 0x00000005u, 0x00000020u, 0x00000049u, 0x00000023u, 0x000300f7u, 0x0000006du, 0x00000000u, 0x000300fbu,
        0x00000023u, 0x0000006eu, 0x000200f8u, 0x0000006eu, 0x00050051u, 0x00000029u, 0x0000006fu, 0x0000006bu, 0x00000003u, 0x000500b4u,
        0x00000024u, 0x00000070u, 0x0000006fu, 0x0000002au, 0x000300f7u, 0x00000071u, 0x00000000u, 0x000400fau, 0x00000070u, 0x00000072u,
        0x00000071u, 0x000200f8u, 0x00000072u, 0x000200f9u, 0x0000006du, 0x000200f8u, 0x00000071u, 0x000500aau, 0x00000024u, 0x00000073u,
        0x00000069u, 0x00000023u, 0x000300f7u, 0x00000074u, 0x00000000u, 0x000400fau, 0x00000073u, 0x00000075u, 0x00000074u, 0x000200f8u,
        0x00000075u, 0x000200f9u, 0x0000006du, 0x000200f8u, 0x00000074u, 0x00050051u, 0x00000029u, 0x00000076u, 0x00000067u, 0x00000003u,
        0x000500b4u, 0x00000024u, 0x00000077u, 0x00000076u, 0x0000002au, 0x000300f7u, 0x00000078u, 0x00000000u, 0x000400fau, 0x00000077u,
        0x00000079u, 0x00000078u, 0x000200f8u, 0x00000079u, 0x000200f9u, 0x0000006du, 0x000200f8u, 0x00000078u, 0x000200f9u, 0x0000007au,
        0x000200f8u, 0x0000007au, 0x000700f5u, 0x00000022u, 0x0000007bu, 0x00000023u, 0x00000078u, 0x0000001eu, 0x0000007cu, 0x000500b0u,
        0x00000024u, 0x0000007du, 0x0000007bu, 0x0000002cu, 0x000400f6u, 0x0000007eu, 0x0000007cu, 0x00000000u, 0x000400fau, 0x0000007du,
        0x0000007fu, 0x0000007eu, 0x000200f8u, 0x0000007fu, 0x00050041u, 0x00000040u, 0x00000080u, 0x0000006cu, 0x0000007bu, 0x0004003du,
        0x0000002du, 0x00000081u, 0x00000080u, 0x0008004fu, 0x0000002eu, 0x00000082u, 0x00000081u, 0x00000081u, 0x00000000u, 0x00000001u,
        0x00000002u, 0x0008004fu, 0x0000002eu, 0x00000083u, 0x00000067u, 0x00000067u, 0x00000000u, 0x00000001u, 0x00000002u, 0x00050094u,
        0x00000029u, 0x0000001au, 0x00000082u, 0x00000083u, 0x00060041u, 0x00000044u, 0x00000084u, 0x0000006cu, 0x0000007bu, 0x00000021u,
        0x0004003du, 0x00000029u, 0x00000085u, 0x00000084u, 0x00050081u, 0x00000029u, 0x0000001bu, 0x0000001au, 0x00000085u, 0x0004007fu,
        0x00000029u, 0x0000001cu, 0x00000076u, 0x0004003du, 0x0000002du, 0x00000086u, 0x00000080u, 0x0008004fu, 0x0000002eu, 0x00000087u,
        0x00000086u, 0x00000086u, 0x00000000u, 0x00000001u, 0x00000002u, 0x0006000cu, 0x00000029u, 0x00000088u, 0x00000001u, 0x00000042u,
        0x00000087u, 0x00050085u, 0x00000029u, 0x0000001du, 0x0000001cu, 0x00000088u, 0x000500b8u, 0x00000024u, 0x00000089u, 0x0000001bu,
        0x0000001du, 0x000300f7u, 0x0000008au, 0x00000000u, 0x000400fau, 0x00000089u, 0x0000008bu, 0x0000008au, 0x000200f8u, 0x0000008bu,
        0x000200f9u, 0x0000007eu, 0x000200f8u, 0x0000008au, 0x000200f9u, 0x0000007cu, 0x000200f8u, 0x0000007cu, 0x00050080u, 0x00000022u,
        0x0000001eu, 0x0000007bu, 0x00000028u, 0x000200f9u, 0x0000007au, 0x000200f8u, 0x0000007eu, 0x000700f5u, 0x00000024u, 0x0000008cu,
        0x00000058u, 0x0000007au, 0x0000002bu, 0x0000008bu, 0x000700f5u, 0x00000024u, 0x0000008du, 0x0000002bu, 0x0000007au, 0x00000025u,
        0x0000008bu, 0x000300f7u, 0x0000008eu, 0x00000000u, 0x000400fau, 0x0000008du, 0x0000006du, 0x0000008eu, 0x000200f8u, 0x0000008eu,
        0x000200f9u, 0x0000006du, 0x000200f8u, 0x0000006du, 0x000d00f5u, 0x00000024u, 0x00000059u, 0x0000002bu, 0x00000072u, 0x00000025u,
        0x00000075u, 0x0000002bu, 0x00000079u, 0x0000008cu, 0x0000007eu, 0x00000025u, 0x0000008eu, 0x000400a8u, 0x00000024u, 0x0000008fu,
        0x00000059u, 0x000300f7u, 0x00000090u, 0x00000000u, 0x000400fau, 0x0000008fu, 0x00000091u, 0x00000090u, 0x000200f8u, 0x00000091u,
        0x000200f9u, 0x0000005au, 0x000200f8u, 0x00000090u, 0x000500aau, 0x00000024u, 0x00000092u, 0x0000005du, 0x00000056u, 0x000300f7u,
        0x00000093u, 0x00000000u, 0x000400fau, 0x00000092u, 0x00000094u, 0x00000095u, 0x000200f8u, 0x00000094u, 0x00050080u, 0x00000022u,
        0x00000016u, 0x0000005bu, 0x00000028u, 0x000200f9u, 0x00000093u, 0x000200f8u, 0x00000095u, 0x00050080u, 0x00000022u, 0x00000017u,
        0x0000005du, 0x00000028u, 0x00050080u, 0x00000022u, 0x00000018u, 0x00000015u, 0x0000005du, 0x00060041u, 0x0000003du, 0x00000096u,
        0x00000007u, 0x00000020u, 0x00000018u, 0x0003003eu, 0x00000096u, 0x0000005fu, 0x000200f9u, 0x00000093u, 0x000200f8u, 0x00000093u,
        0x000700f5u, 0x00000022u, 0x00000097u, 0x00000016u, 0x00000094u, 0x0000005bu, 0x00000095u, 0x000700f5u, 0x00000022u, 0x00000098u,
        0x0000005du, 0x00000094u, 0x00000017u, 0x00000095u, 0x000200f9u, 0x0000005au, 0x000200f8u, 0x0000005au, 0x000700f5u, 0x00000022u,
        0x0000005cu, 0x0000005bu, 0x00000091u, 0x00000097u, 0x00000093u, 0x000700f5u, 0x00000022u, 0x0000005eu, 0x0000005du, 0x00000091u,
        0x00000098u, 0x00000093u, 0x00050080u, 0x00000022u, 0x00000019u, 0x0000005fu, 0x00000028u, 0x000200f9u, 0x00000057u, 0x000200f8u,
        0x00000063u, 0x00070050u, 0x00000033u, 0x00000099u, 0x00000015u, 0x0000005du, 0x0000005bu, 0x00000023u, 0x00060041u, 0x0000003eu,
        0x0000009au, 0x00000006u, 0x00000020u, 0x00000049u, 0x0003003eu, 0x0000009au, 0x00000099u, 0x000200f9u, 0x00000047u, 0x000200f8u,
        0x00000047u, 0x000100fdu, 0x00010038u,
    };
    inline constexpr std::string_view LightCullingCapturedMetal = R"CAPTURE(#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T>
[[clang::optnone]] T spvFMul(T l, T r)
{
    return fma(l, r, T(0));
}

template<typename T, int Cols, int Rows>
[[clang::optnone]] vec<T, Cols> spvFMulVectorMatrix(vec<T, Rows> v, matrix<T, Cols, Rows> m)
{
    vec<T, Cols> res = vec<T, Cols>(0);
    for (uint i = Rows; i > 0; --i)
    {
        vec<T, Cols> tmp(0);
        for (uint j = 0; j < Cols; ++j)
        {
            tmp[j] = m[j][i - 1];
        }
        res = fma(tmp, vec<T, Cols>(v[i - 1]), res);
    }
    return res;
}

template<typename T, int Cols, int Rows>
[[clang::optnone]] vec<T, Rows> spvFMulMatrixVector(matrix<T, Cols, Rows> m, vec<T, Cols> v)
{
    vec<T, Rows> res = vec<T, Rows>(0);
    for (uint i = Cols; i > 0; --i)
    {
        res = fma(m[i - 1], vec<T, Rows>(v[i - 1]), res);
    }
    return res;
}

template<typename T, int LCols, int LRows, int RCols, int RRows>
[[clang::optnone]] matrix<T, RCols, LRows> spvFMulMatrixMatrix(matrix<T, LCols, LRows> l, matrix<T, RCols, RRows> r)
{
    static_assert(LCols == RRows, "column-row configuration mismatch");
    matrix<T, RCols, LRows> res;
    for (uint i = 0; i < RCols; i++)
    {
        vec<T, LRows> tmp(0);
        for (uint j = 0; j < LCols; j++)
        {
            tmp = fma(vec<T, LRows>(r[i][j]), l[j], tmp);
        }
        res[i] = tmp;
    }
    return res;
}

template<typename T>
[[clang::optnone]] T spvFAdd(T l, T r)
{
    return fma(T(1), l, r);
}

struct PackedLight
{
    float4 positionRange;
    packed_float3 direction;
    uint kind;
    float4 colorIntensity;
    float2 cones;
    uint2 identity;
};

struct type_StructuredBuffer_PackedLight
{
    PackedLight _m0[1];
};

struct Cluster
{
    float4 planes[6];
};

struct type_StructuredBuffer_Cluster
{
    Cluster _m0[1];
};

struct type_RWStructuredBuffer_v4uint
{
    uint4 _m0[1];
};

struct type_RWStructuredBuffer_uint
{
    uint _m0[1];
};

struct type_Dispatch
{
    uint LightCount;
    uint ClusterCount;
    uint ReferencesPerCluster;
    uint Reserved;
};

constant bool _63 = {};

kernel void CullLights(device type_StructuredBuffer_PackedLight& Lights [[buffer(0)]], device type_StructuredBuffer_Cluster& Clusters [[buffer(1)]], device type_RWStructuredBuffer_v4uint& Membership [[buffer(2)]], device type_RWStructuredBuffer_uint& References [[buffer(3)]], constant type_Dispatch& Dispatch [[buffer(4)]], uint3 gl_GlobalInvocationID [[thread_position_in_grid]])
{
    do
    {
        bool _82;
        if (Dispatch.Reserved == 0u)
        {
            _82 = gl_GlobalInvocationID.x >= Dispatch.ClusterCount;
        }
        else
        {
            _82 = true;
        }
        if (_82)
        {
            break;
        }
        uint _21 = gl_GlobalInvocationID.x * Dispatch.ReferencesPerCluster;
        uint _91;
        uint _93;
        _91 = 0u;
        _93 = 0u;
        bool _89;
        uint _92;
        uint _94;
        bool _88;
        for (uint _95 = 0u; _95 < Dispatch.LightCount; _88 = _89, _91 = _92, _93 = _94, _95++)
        {
            do
            {
                if (Lights._m0[_95].colorIntensity.w == 0.0)
                {
                    _89 = false;
                    break;
                }
                if (Lights._m0[_95].kind == 0u)
                {
                    _89 = true;
                    break;
                }
                if (Lights._m0[_95].positionRange.w == 0.0)
                {
                    _89 = false;
                    break;
                }
                bool _140;
                bool _141;
                uint _123 = 0u;
                for (;;)
                {
                    if (_123 < 6u)
                    {
                        if (spvFAdd(dot(Clusters._m0[gl_GlobalInvocationID.x].planes[_123].xyz, Lights._m0[_95].positionRange.xyz), ((device float*)&Clusters._m0[gl_GlobalInvocationID.x].planes[_123])[3]) < spvFMul(-Lights._m0[_95].positionRange.w, length(Clusters._m0[gl_GlobalInvocationID.x].planes[_123].xyz)))
                        {
                            _140 = false;
                            _141 = true;
                            break;
                        }
                        _123++;
                        continue;
                    }
                    else
                    {
                        _140 = _88;
                        _141 = false;
                        break;
                    }
                }
                if (_141)
                {
                    _89 = _140;
                    break;
                }
                _89 = true;
                break;
            } while(false);
            if (!_89)
            {
                _92 = _91;
                _94 = _93;
                continue;
            }
            uint _151;
            uint _152;
            if (_93 == Dispatch.ReferencesPerCluster)
            {
                _151 = _91 + 1u;
                _152 = _93;
            }
            else
            {
                References._m0[_21 + _93] = _95;
                _151 = _91;
                _152 = _93 + 1u;
            }
            _92 = _151;
            _94 = _152;
        }
        Membership._m0[gl_GlobalInvocationID.x] = uint4(_21, _93, _91, 0u);
        break;
    } while(false);
}

)CAPTURE";
    inline constexpr std::string_view LightCullingCapturedJson = R"CAPTURE({
    "entryPoints" : [
        {
            "name" : "CullLights",
            "mode" : "comp",
            "workgroup_size" : [
                64,
                1,
                1
            ],
            "workgroup_size_is_spec_constant_id" : [
                false,
                false,
                false
            ]
        }
    ],
    "types" : {
        "_10" : {
            "name" : "PackedLight",
            "members" : [
                {
                    "name" : "positionRange",
                    "type" : "vec4",
                    "offset" : 0
                },
                {
                    "name" : "direction",
                    "type" : "vec3",
                    "offset" : 16
                },
                {
                    "name" : "kind",
                    "type" : "uint",
                    "offset" : 28
                },
                {
                    "name" : "colorIntensity",
                    "type" : "vec4",
                    "offset" : 32
                },
                {
                    "name" : "cones",
                    "type" : "vec2",
                    "offset" : 48
                },
                {
                    "name" : "identity",
                    "type" : "uvec2",
                    "offset" : 56
                }
            ]
        },
        "_9" : {
            "name" : "type.StructuredBuffer.PackedLight",
            "members" : [
                {
                    "name" : "_m0",
                    "type" : "_10",
                    "array" : [
                        0
                    ],
                    "array_size_is_literal" : [
                        true
                    ],
                    "offset" : 0,
                    "array_stride" : 64
                }
            ]
        },
        "_12" : {
            "name" : "Cluster",
            "members" : [
                {
                    "name" : "planes",
                    "type" : "vec4",
                    "array" : [
                        6
                    ],
                    "array_size_is_literal" : [
                        true
                    ],
                    "offset" : 0,
                    "array_stride" : 16
                }
            ]
        },
        "_11" : {
            "name" : "type.StructuredBuffer.Cluster",
            "members" : [
                {
                    "name" : "_m0",
                    "type" : "_12",
                    "array" : [
                        0
                    ],
                    "array_size_is_literal" : [
                        true
                    ],
                    "offset" : 0,
                    "array_stride" : 96
                }
            ]
        },
        "_13" : {
            "name" : "type.RWStructuredBuffer.v4uint",
            "members" : [
                {
                    "name" : "_m0",
                    "type" : "uvec4",
                    "array" : [
                        0
                    ],
                    "array_size_is_literal" : [
                        true
                    ],
                    "offset" : 0,
                    "array_stride" : 16
                }
            ]
        },
        "_14" : {
            "name" : "type.RWStructuredBuffer.uint",
            "members" : [
                {
                    "name" : "_m0",
                    "type" : "uint",
                    "array" : [
                        0
                    ],
                    "array_size_is_literal" : [
                        true
                    ],
                    "offset" : 0,
                    "array_stride" : 4
                }
            ]
        },
        "_15" : {
            "name" : "type.Dispatch",
            "members" : [
                {
                    "name" : "LightCount",
                    "type" : "uint",
                    "offset" : 0
                },
                {
                    "name" : "ClusterCount",
                    "type" : "uint",
                    "offset" : 4
                },
                {
                    "name" : "ReferencesPerCluster",
                    "type" : "uint",
                    "offset" : 8
                },
                {
                    "name" : "Reserved",
                    "type" : "uint",
                    "offset" : 12
                }
            ]
        }
    },
    "ssbos" : [
        {
            "type" : "_9",
            "name" : "Lights",
            "readonly" : true,
            "block_size" : 0,
            "set" : 0,
            "binding" : 0
        },
        {
            "type" : "_11",
            "name" : "Clusters",
            "readonly" : true,
            "block_size" : 0,
            "set" : 0,
            "binding" : 1
        },
        {
            "type" : "_13",
            "name" : "Membership",
            "block_size" : 0,
            "set" : 0,
            "binding" : 2
        },
        {
            "type" : "_14",
            "name" : "References",
            "block_size" : 0,
            "set" : 0,
            "binding" : 3
        }
    ],
    "ubos" : [
        {
            "type" : "_15",
            "name" : "type.Dispatch",
            "block_size" : 16,
            "set" : 0,
            "binding" : 4
        }
    ]
})CAPTURE";
}  // namespace Horo::Tests
