// SPDX-License-Identifier: MIT
// Adapted from bcdec by Sergii Kudlai (2022), revision
// 80859ed3b7afb1c527a2a99d70c61457bea72d0c, https://github.com/iOrange/bcdec.
// The partition_id/anchor data and endpoint rules follow bcdec_bc7; this port
// decodes one texel using 32-bit word access and direct index offsets.
// See LICENSE.bcdec for the complete notice.

// Two bits per texel select a subset. The second word marks anchor texels,
// whose primary indices omit one bit. Entries 0..63: two subsets; 64..127: three.
const uvec2 bc7_partitions[128] = uvec2[128](
    uvec2(0x50505050u, 0x8001u),
    uvec2(0x40404040u, 0x8001u),
    uvec2(0x54545454u, 0x8001u),
    uvec2(0x54505040u, 0x8001u),
    uvec2(0x50404000u, 0x8001u),
    uvec2(0x55545450u, 0x8001u),
    uvec2(0x55545040u, 0x8001u),
    uvec2(0x54504000u, 0x8001u),
    uvec2(0x50400000u, 0x8001u),
    uvec2(0x55555450u, 0x8001u),
    uvec2(0x55544000u, 0x8001u),
    uvec2(0x54400000u, 0x8001u),
    uvec2(0x55555440u, 0x8001u),
    uvec2(0x55550000u, 0x8001u),
    uvec2(0x55555500u, 0x8001u),
    uvec2(0x55000000u, 0x8001u),
    uvec2(0x55150100u, 0x8001u),
    uvec2(0x00004054u, 0x0005u),
    uvec2(0x15010000u, 0x0101u),
    uvec2(0x00405054u, 0x0005u),
    uvec2(0x00004050u, 0x0005u),
    uvec2(0x15050100u, 0x0101u),
    uvec2(0x05010000u, 0x0101u),
    uvec2(0x40505054u, 0x8001u),
    uvec2(0x00404050u, 0x0005u),
    uvec2(0x05010100u, 0x0101u),
    uvec2(0x14141414u, 0x0005u),
    uvec2(0x05141450u, 0x0005u),
    uvec2(0x01155440u, 0x0101u),
    uvec2(0x00555500u, 0x0101u),
    uvec2(0x15014054u, 0x0005u),
    uvec2(0x05414150u, 0x0005u),
    uvec2(0x44444444u, 0x8001u),
    uvec2(0x55005500u, 0x8001u),
    uvec2(0x11441144u, 0x0041u),
    uvec2(0x05055050u, 0x0101u),
    uvec2(0x05500550u, 0x0005u),
    uvec2(0x11114444u, 0x0101u),
    uvec2(0x41144114u, 0x8001u),
    uvec2(0x44111144u, 0x8001u),
    uvec2(0x15055054u, 0x0005u),
    uvec2(0x01055040u, 0x0101u),
    uvec2(0x05041050u, 0x0005u),
    uvec2(0x05455150u, 0x0005u),
    uvec2(0x14414114u, 0x0005u),
    uvec2(0x50050550u, 0x8001u),
    uvec2(0x41411414u, 0x8001u),
    uvec2(0x00141400u, 0x0041u),
    uvec2(0x00041504u, 0x0041u),
    uvec2(0x00105410u, 0x0005u),
    uvec2(0x10541000u, 0x0041u),
    uvec2(0x04150400u, 0x0101u),
    uvec2(0x50410514u, 0x8001u),
    uvec2(0x41051450u, 0x8001u),
    uvec2(0x05415014u, 0x0005u),
    uvec2(0x14054150u, 0x0005u),
    uvec2(0x41050514u, 0x8001u),
    uvec2(0x41505014u, 0x8001u),
    uvec2(0x40011554u, 0x8001u),
    uvec2(0x54150140u, 0x8001u),
    uvec2(0x50505500u, 0x8001u),
    uvec2(0x00555050u, 0x0005u),
    uvec2(0x15151010u, 0x0005u),
    uvec2(0x54540404u, 0x8001u),
    uvec2(0xaa685050u, 0x8009u),
    uvec2(0x6a5a5040u, 0x0109u),
    uvec2(0x5a5a4200u, 0x8101u),
    uvec2(0x5450a0a8u, 0x8009u),
    uvec2(0xa5a50000u, 0x8101u),
    uvec2(0xa0a05050u, 0x8009u),
    uvec2(0x5555a0a0u, 0x8009u),
    uvec2(0x5a5a5050u, 0x8101u),
    uvec2(0xaa550000u, 0x8101u),
    uvec2(0xaa555500u, 0x8101u),
    uvec2(0xaaaa5500u, 0x8041u),
    uvec2(0x90909090u, 0x8041u),
    uvec2(0x94949494u, 0x8041u),
    uvec2(0xa4a4a4a4u, 0x8021u),
    uvec2(0xa9a59450u, 0x8009u),
    uvec2(0x2a0a4250u, 0x0109u),
    uvec2(0xa5945040u, 0x8009u),
    uvec2(0x0a425054u, 0x0109u),
    uvec2(0xa5a5a500u, 0x8101u),
    uvec2(0x55a0a0a0u, 0x8009u),
    uvec2(0xa8a85454u, 0x8009u),
    uvec2(0x6a6a4040u, 0x0109u),
    uvec2(0xa4a45000u, 0x8041u),
    uvec2(0x1a1a0500u, 0x0501u),
    uvec2(0x0050a4a4u, 0x0029u),
    uvec2(0xaaa59090u, 0x8101u),
    uvec2(0x14696914u, 0x0141u),
    uvec2(0x69691400u, 0x0441u),
    uvec2(0xa08585a0u, 0x8101u),
    uvec2(0xaa821414u, 0x8021u),
    uvec2(0x50a4a450u, 0x8401u),
    uvec2(0x6a5a0200u, 0x8101u),
    uvec2(0xa9a58000u, 0x8101u),
    uvec2(0x5090a0a8u, 0x8009u),
    uvec2(0xa8a09050u, 0x8009u),
    uvec2(0x24242424u, 0x0421u),
    uvec2(0x00aa5500u, 0x0441u),
    uvec2(0x24924924u, 0x0501u),
    uvec2(0x24499224u, 0x0301u),
    uvec2(0x50a50a50u, 0x8401u),
    uvec2(0x500aa550u, 0x8041u),
    uvec2(0xaaaa4444u, 0x8009u),
    uvec2(0x66660000u, 0x8101u),
    uvec2(0xa5a0a5a0u, 0x8021u),
    uvec2(0x50a050a0u, 0x8009u),
    uvec2(0x69286928u, 0x8041u),
    uvec2(0x44aaaa44u, 0x8041u),
    uvec2(0x66666600u, 0x8101u),
    uvec2(0xaa444444u, 0x8009u),
    uvec2(0x54a854a8u, 0x8009u),
    uvec2(0x95809580u, 0x8021u),
    uvec2(0x96969600u, 0x8021u),
    uvec2(0xa85454a8u, 0x8021u),
    uvec2(0x80959580u, 0x8101u),
    uvec2(0xaa141414u, 0x8021u),
    uvec2(0x96960000u, 0x8401u),
    uvec2(0xaaaa1414u, 0x8021u),
    uvec2(0xa05050a0u, 0x8401u),
    uvec2(0xa0a5a5a0u, 0x8101u),
    uvec2(0x96000000u, 0xa001u),
    uvec2(0x40804080u, 0x8009u),
    uvec2(0xa9a8a9a8u, 0x9001u),
    uvec2(0xaaaaaa44u, 0x8009u),
    uvec2(0x2a4a5254u, 0x0109u)
);

// Every valid field is at most eight bits and lies within the 128-bit block.
// Read the next word only on a crossing; neither shift can equal 32.
uint bc7_bits(uvec4 block, uint offset, uint count)
{
    uint word = offset >> 5, shift = offset & 31u;
    uint value = block[word] >> shift;
    if (shift + count > 32u) value |= block[word + 1u] << (32u - shift);
    return value & ((1u << count) - 1u);
}
uint bc7_read(uvec4 block, inout uint offset, uint count)
{
    uint value = bc7_bits(block, offset, count);
    offset += count;
    return value;
}
uint bc7_weight(uint bits, uint index)
{
    const uint weights2[4] = uint[4](0, 21, 43, 64);
    const uint weights3[8] = uint[8](0, 9, 18, 27, 37, 46, 55, 64);
    const uint weights4[16] = uint[16](0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64);
    return bits == 2u ? weights2[index] : bits == 3u ? weights3[index] : weights4[index];
}
uint bc7_rgba(uvec4 block, uint pixel)
{
    int first = findLSB(block.x & 255u);
    // Reserved mode: retain bcdec's deterministic transparent-black result.
    if (first < 0) return 0;
    uint mode = uint(first), offset = mode + 1u;
    const uint color_bits[8] = uint[8](4, 6, 5, 7, 5, 7, 7, 5);
    const uint alpha_bits[8] = uint[8](0, 0, 0, 0, 6, 8, 7, 5);
    uint subsets = 1u, partition_id = 0u, rotation = 0u, selection = 0u;
    if (mode < 4u || mode == 7u) {
        subsets = mode == 0u || mode == 2u ? 3u : 2u;
        partition_id = bc7_read(block, offset, mode == 0u ? 4u : 6u);
    }
    if (mode == 4u || mode == 5u) {
        rotation = bc7_read(block, offset, 2u);
        if (mode == 4u) selection = bc7_read(block, offset, 1u);
    }
    uint endpoints_count = subsets * 2u;
    uvec4 endpoints[6];
    for (uint i = 0u; i < endpoints_count; ++i) endpoints[i] = uvec4(0u);
    for (uint component = 0u; component < 3u; ++component)
        for (uint i = 0u; i < endpoints_count; ++i)
            endpoints[i][component] = bc7_read(block, offset, color_bits[mode]);
    if (alpha_bits[mode] != 0u)
        for (uint i = 0u; i < endpoints_count; ++i)
            endpoints[i].a = bc7_read(block, offset, alpha_bits[mode]);
    uint pbit = (0xcbu >> mode) & 1u;
    if (pbit != 0u) {
        if (mode == 1u) {
            for (uint subset = 0u; subset < subsets; ++subset) {
                uint bit = bc7_read(block, offset, 1u);
                endpoints[2u * subset] = (endpoints[2u * subset] << 1u) | uvec4(bit);
                endpoints[2u * subset + 1u] = (endpoints[2u * subset + 1u] << 1u) | uvec4(bit);
            }
        } else {
            for (uint i = 0u; i < endpoints_count; ++i) {
                uint bit = bc7_read(block, offset, 1u);
                endpoints[i] = (endpoints[i] << 1u) | uvec4(bit);
            }
        }
    }
    for (uint i = 0u; i < endpoints_count; ++i) {
        uint bits = color_bits[mode] + pbit;
        endpoints[i].rgb <<= 8u - bits;
        endpoints[i].rgb |= endpoints[i].rgb >> bits;
        if (alpha_bits[mode] == 0u) endpoints[i].a = 255u;
        else {
            bits = alpha_bits[mode] + pbit;
            endpoints[i].a <<= 8u - bits;
            endpoints[i].a |= endpoints[i].a >> bits;
        }
    }
    uvec2 shape = subsets == 1u ? uvec2(0u, 1u) : bc7_partitions[(subsets - 2u) * 64u + partition_id];
    uint subset = (shape.x >> (pixel * 2u)) & 3u;
    uint index_bits = mode < 2u ? 3u : mode == 6u ? 4u : 2u;
    uint secondary_bits = mode == 4u ? 3u : mode == 5u ? 2u : 0u;
    uint preceding_anchors = uint(bitCount(shape.y & ((1u << pixel) - 1u)));
    uint anchor = (shape.y >> pixel) & 1u;
    uint index = bc7_bits(block, offset + pixel * index_bits - preceding_anchors, index_bits - anchor);
    uint weight = bc7_weight(index_bits, index), alpha_weight = weight;
    if (secondary_bits != 0u) {
        uint secondary_start = offset + 16u * index_bits - subsets;
        uint secondary_index = bc7_bits(block, secondary_start + pixel * secondary_bits - (pixel == 0u ? 0u : 1u),
            secondary_bits - (pixel == 0u ? 1u : 0u));
        uint secondary_weight = bc7_weight(secondary_bits, secondary_index);
        if (selection == 0u) alpha_weight = secondary_weight;
        else weight = secondary_weight;
    }
    uvec4 a = endpoints[2u * subset], b = endpoints[2u * subset + 1u];
    uvec4 weights = uvec4(weight, weight, weight, alpha_weight);
    uvec4 result = ((64u - weights) * a + weights * b + 32u) >> 6u;
    if (rotation != 0u) {
        uint scalar = result.a;
        result.a = result[rotation - 1u];
        result[rotation - 1u] = scalar;
    }
    return result.r | (result.g << 8u) | (result.b << 16u) | (result.a << 24u);
}
