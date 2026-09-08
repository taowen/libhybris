// SPDX-License-Identifier: MIT
// Adapted from bcdec by Sergii Kudlai (2022), revision
// 80859ed3b7afb1c527a2a99d70c61457bea72d0c, https://github.com/iOrange/bcdec.
// See LICENSE.bcdec. Endpoint unpacking follows bcdec_bc6h_half. Final signed
// scaling preserves the original sign even when the magnitude rounds to zero.
// BPTC bit access, weights and two-subset partition shapes are shared with BC7.
int bc6_read(uvec4 block, inout uint offset, int count)
{ return int(bc7_read(block, offset, uint(count))); }
int bc6_read_reverse(uvec4 block, inout uint offset, int count)
{ return int(bitfieldReverse(uint(bc6_read(block, offset, count))) >> (32 - count)); }
int bc6_sign(int value, int bits)
{ return int(uint(value) << (32 - bits)) >> (32 - bits); }
int bc6_unquantize(int value, int bits, bool signed_format)
{
    if (!signed_format) {
        if (bits >= 15) return value;
        if (value == 0) return 0;
        if (value == (1 << bits) - 1) return 65535;
        return ((value << 16) + 32768) >> bits;
    }
    if (bits >= 16) return value;
    int magnitude = abs(value);
    if (magnitude == 0) return 0;
    int expanded = magnitude >= (1 << (bits - 1)) - 1 ? 32767 :
        ((magnitude << 15) + 16384) >> (bits - 1);
    return value < 0 ? -expanded : expanded;
}
uint bc6_finish(int value, bool signed_format)
{
    if (!signed_format) return uint((value * 31) >> 6);
    return uint((abs(value) * 31) >> 5) | (value < 0 ? 0x8000u : 0u);
}
uvec2 bc6_rgba16(uvec4 block, uint pixel, bool signed_format)
{
    const ivec4 endpoint_bits[14] = ivec4[14](
        ivec4(10,5,5,5), ivec4(7,6,6,6), ivec4(11,5,4,4),
        ivec4(11,4,5,4), ivec4(11,4,4,5), ivec4(9,5,5,5),
        ivec4(8,6,5,5), ivec4(8,5,6,5), ivec4(8,5,5,6),
        ivec4(6,6,6,6), ivec4(10,10,10,10), ivec4(11,9,9,9),
        ivec4(12,8,8,8), ivec4(16,4,4,4));
    int r[4] = int[4](0,0,0,0), g[4] = int[4](0,0,0,0), b[4] = int[4](0,0,0,0);
    uint offset = 0u;
    int mode = bc6_read(block, offset, 2);
    if (mode > 1) mode |= bc6_read(block, offset, 3) << 2;
    int partition_id = 0;
    switch (mode) {
        /* mode 1 */
        case 0: {
            /* Partitition indices: 46 bits
               Partition: 5 bits
               Color Endpoints: 75 bits (10.555, 10.555, 10.555) */
            g[2] |= bc6_read(block, offset, 1) << 4;       /* gy[4]   */
            b[2] |= bc6_read(block, offset, 1) << 4;       /* by[4]   */
            b[3] |= bc6_read(block, offset, 1) << 4;       /* bz[4]   */
            r[0] |= bc6_read(block, offset, 10);       /* rw[9:0] */
            g[0] |= bc6_read(block, offset, 10);       /* gw[9:0] */
            b[0] |= bc6_read(block, offset, 10);       /* bw[9:0] */
            r[1] |= bc6_read(block, offset, 5);        /* rx[4:0] */
            g[3] |= bc6_read(block, offset, 1) << 4;       /* gz[4]   */
            g[2] |= bc6_read(block, offset, 4);        /* gy[3:0] */
            g[1] |= bc6_read(block, offset, 5);        /* gx[4:0] */
            b[3] |= bc6_read(block, offset, 1);            /* bz[0]   */
            g[3] |= bc6_read(block, offset, 4);        /* gz[3:0] */
            b[1] |= bc6_read(block, offset, 5);        /* bx[4:0] */
            b[3] |= bc6_read(block, offset, 1) << 1;       /* bz[1]   */
            b[2] |= bc6_read(block, offset, 4);        /* by[3:0] */
            r[2] |= bc6_read(block, offset, 5);        /* ry[4:0] */
            b[3] |= bc6_read(block, offset, 1) << 2;       /* bz[2]   */
            r[3] |= bc6_read(block, offset, 5);        /* rz[4:0] */
            b[3] |= bc6_read(block, offset, 1) << 3;       /* bz[3]   */
            partition_id = bc6_read(block, offset, 5);    /* d[4:0]  */
            mode = 0;
        } break;

        /* mode 2 */
        case 1: {
            /* Partitition indices: 46 bits
               Partition: 5 bits
               Color Endpoints: 75 bits (7666, 7666, 7666) */
            g[2] |= bc6_read(block, offset, 1) << 5;       /* gy[5]   */
            g[3] |= bc6_read(block, offset, 1) << 4;       /* gz[4]   */
            g[3] |= bc6_read(block, offset, 1) << 5;       /* gz[5]   */
            r[0] |= bc6_read(block, offset, 7);        /* rw[6:0] */
            b[3] |= bc6_read(block, offset, 1);            /* bz[0]   */
            b[3] |= bc6_read(block, offset, 1) << 1;       /* bz[1]   */
            b[2] |= bc6_read(block, offset, 1) << 4;       /* by[4]   */
            g[0] |= bc6_read(block, offset, 7);        /* gw[6:0] */
            b[2] |= bc6_read(block, offset, 1) << 5;       /* by[5]   */
            b[3] |= bc6_read(block, offset, 1) << 2;       /* bz[2]   */
            g[2] |= bc6_read(block, offset, 1) << 4;       /* gy[4]   */
            b[0] |= bc6_read(block, offset, 7);        /* bw[6:0] */
            b[3] |= bc6_read(block, offset, 1) << 3;       /* bz[3]   */
            b[3] |= bc6_read(block, offset, 1) << 5;       /* bz[5]   */
            b[3] |= bc6_read(block, offset, 1) << 4;       /* bz[4]   */
            r[1] |= bc6_read(block, offset, 6);        /* rx[5:0] */
            g[2] |= bc6_read(block, offset, 4);        /* gy[3:0] */
            g[1] |= bc6_read(block, offset, 6);        /* gx[5:0] */
            g[3] |= bc6_read(block, offset, 4);        /* gz[3:0] */
            b[1] |= bc6_read(block, offset, 6);        /* bx[5:0] */
            b[2] |= bc6_read(block, offset, 4);        /* by[3:0] */
            r[2] |= bc6_read(block, offset, 6);        /* ry[5:0] */
            r[3] |= bc6_read(block, offset, 6);        /* rz[5:0] */
            partition_id = bc6_read(block, offset, 5);    /* d[4:0]  */
            mode = 1;
        } break;

        /* mode 3 */
        case 2: {
            /* Partitition indices: 46 bits
               Partition: 5 bits
               Color Endpoints: 72 bits (11.555, 11.444, 11.444) */
            r[0] |= bc6_read(block, offset, 10);       /* rw[9:0] */
            g[0] |= bc6_read(block, offset, 10);       /* gw[9:0] */
            b[0] |= bc6_read(block, offset, 10);       /* bw[9:0] */
            r[1] |= bc6_read(block, offset, 5);        /* rx[4:0] */
            r[0] |= bc6_read(block, offset, 1) << 10;      /* rw[10]  */
            g[2] |= bc6_read(block, offset, 4);        /* gy[3:0] */
            g[1] |= bc6_read(block, offset, 4);        /* gx[3:0] */
            g[0] |= bc6_read(block, offset, 1) << 10;      /* gw[10]  */
            b[3] |= bc6_read(block, offset, 1);            /* bz[0]   */
            g[3] |= bc6_read(block, offset, 4);        /* gz[3:0] */
            b[1] |= bc6_read(block, offset, 4);        /* bx[3:0] */
            b[0] |= bc6_read(block, offset, 1) << 10;      /* bw[10]  */
            b[3] |= bc6_read(block, offset, 1) << 1;       /* bz[1]   */
            b[2] |= bc6_read(block, offset, 4);        /* by[3:0] */
            r[2] |= bc6_read(block, offset, 5);        /* ry[4:0] */
            b[3] |= bc6_read(block, offset, 1) << 2;       /* bz[2]   */
            r[3] |= bc6_read(block, offset, 5);        /* rz[4:0] */
            b[3] |= bc6_read(block, offset, 1) << 3;       /* bz[3]   */
            partition_id = bc6_read(block, offset, 5);    /* d[4:0]  */
            mode = 2;
        } break;

        /* mode 4 */
        case 6: {
            /* Partitition indices: 46 bits
               Partition: 5 bits
               Color Endpoints: 72 bits (11.444, 11.555, 11.444) */
            r[0] |= bc6_read(block, offset, 10);       /* rw[9:0] */
            g[0] |= bc6_read(block, offset, 10);       /* gw[9:0] */
            b[0] |= bc6_read(block, offset, 10);       /* bw[9:0] */
            r[1] |= bc6_read(block, offset, 4);        /* rx[3:0] */
            r[0] |= bc6_read(block, offset, 1) << 10;      /* rw[10]  */
            g[3] |= bc6_read(block, offset, 1) << 4;       /* gz[4]   */
            g[2] |= bc6_read(block, offset, 4);        /* gy[3:0] */
            g[1] |= bc6_read(block, offset, 5);        /* gx[4:0] */
            g[0] |= bc6_read(block, offset, 1) << 10;      /* gw[10]  */
            g[3] |= bc6_read(block, offset, 4);        /* gz[3:0] */
            b[1] |= bc6_read(block, offset, 4);        /* bx[3:0] */
            b[0] |= bc6_read(block, offset, 1) << 10;      /* bw[10]  */
            b[3] |= bc6_read(block, offset, 1) << 1;       /* bz[1]   */
            b[2] |= bc6_read(block, offset, 4);        /* by[3:0] */
            r[2] |= bc6_read(block, offset, 4);        /* ry[3:0] */
            b[3] |= bc6_read(block, offset, 1);            /* bz[0]   */
            b[3] |= bc6_read(block, offset, 1) << 2;       /* bz[2]   */
            r[3] |= bc6_read(block, offset, 4);        /* rz[3:0] */
            g[2] |= bc6_read(block, offset, 1) << 4;       /* gy[4]   */
            b[3] |= bc6_read(block, offset, 1) << 3;       /* bz[3]   */
            partition_id = bc6_read(block, offset, 5);    /* d[4:0]  */
            mode = 3;
        } break;

        /* mode 5 */
        case 10: {
            /* Partitition indices: 46 bits
               Partition: 5 bits
               Color Endpoints: 72 bits (11.444, 11.444, 11.555) */
            r[0] |= bc6_read(block, offset, 10);       /* rw[9:0] */
            g[0] |= bc6_read(block, offset, 10);       /* gw[9:0] */
            b[0] |= bc6_read(block, offset, 10);       /* bw[9:0] */
            r[1] |= bc6_read(block, offset, 4);        /* rx[3:0] */
            r[0] |= bc6_read(block, offset, 1) << 10;      /* rw[10]  */
            b[2] |= bc6_read(block, offset, 1) << 4;       /* by[4]   */
            g[2] |= bc6_read(block, offset, 4);        /* gy[3:0] */
            g[1] |= bc6_read(block, offset, 4);        /* gx[3:0] */
            g[0] |= bc6_read(block, offset, 1) << 10;      /* gw[10]  */
            b[3] |= bc6_read(block, offset, 1);            /* bz[0]   */
            g[3] |= bc6_read(block, offset, 4);        /* gz[3:0] */
            b[1] |= bc6_read(block, offset, 5);        /* bx[4:0] */
            b[0] |= bc6_read(block, offset, 1) << 10;      /* bw[10]  */
            b[2] |= bc6_read(block, offset, 4);        /* by[3:0] */
            r[2] |= bc6_read(block, offset, 4);        /* ry[3:0] */
            b[3] |= bc6_read(block, offset, 1) << 1;       /* bz[1]   */
            b[3] |= bc6_read(block, offset, 1) << 2;       /* bz[2]   */
            r[3] |= bc6_read(block, offset, 4);        /* rz[3:0] */
            b[3] |= bc6_read(block, offset, 1) << 4;       /* bz[4]   */ 
            b[3] |= bc6_read(block, offset, 1) << 3;       /* bz[3]   */
            partition_id = bc6_read(block, offset, 5);    /* d[4:0]  */
            mode = 4;
        } break;

        /* mode 6 */
        case 14: {
            /* Partitition indices: 46 bits
               Partition: 5 bits
               Color Endpoints: 72 bits (9555, 9555, 9555) */
            r[0] |= bc6_read(block, offset, 9);        /* rw[8:0] */
            b[2] |= bc6_read(block, offset, 1) << 4;       /* by[4]   */
            g[0] |= bc6_read(block, offset, 9);        /* gw[8:0] */
            g[2] |= bc6_read(block, offset, 1) << 4;       /* gy[4]   */
            b[0] |= bc6_read(block, offset, 9);        /* bw[8:0] */
            b[3] |= bc6_read(block, offset, 1) << 4;       /* bz[4]   */
            r[1] |= bc6_read(block, offset, 5);        /* rx[4:0] */
            g[3] |= bc6_read(block, offset, 1) << 4;       /* gz[4]   */
            g[2] |= bc6_read(block, offset, 4);        /* gy[3:0] */
            g[1] |= bc6_read(block, offset, 5);        /* gx[4:0] */
            b[3] |= bc6_read(block, offset, 1);            /* bz[0]   */
            g[3] |= bc6_read(block, offset, 4);        /* gx[3:0] */
            b[1] |= bc6_read(block, offset, 5);        /* bx[4:0] */
            b[3] |= bc6_read(block, offset, 1) << 1;       /* bz[1]   */
            b[2] |= bc6_read(block, offset, 4);        /* by[3:0] */
            r[2] |= bc6_read(block, offset, 5);        /* ry[4:0] */
            b[3] |= bc6_read(block, offset, 1) << 2;       /* bz[2]   */
            r[3] |= bc6_read(block, offset, 5);        /* rz[4:0] */
            b[3] |= bc6_read(block, offset, 1) << 3;       /* bz[3]   */
            partition_id = bc6_read(block, offset, 5);    /* d[4:0]  */
            mode = 5;
        } break;

        /* mode 7 */
        case 18: {
            /* Partitition indices: 46 bits
               Partition: 5 bits
               Color Endpoints: 72 bits (8666, 8555, 8555) */
            r[0] |= bc6_read(block, offset, 8);        /* rw[7:0] */
            g[3] |= bc6_read(block, offset, 1) << 4;       /* gz[4]   */
            b[2] |= bc6_read(block, offset, 1) << 4;       /* by[4]   */
            g[0] |= bc6_read(block, offset, 8);        /* gw[7:0] */
            b[3] |= bc6_read(block, offset, 1) << 2;       /* bz[2]   */
            g[2] |= bc6_read(block, offset, 1) << 4;       /* gy[4]   */
            b[0] |= bc6_read(block, offset, 8);        /* bw[7:0] */
            b[3] |= bc6_read(block, offset, 1) << 3;       /* bz[3]   */
            b[3] |= bc6_read(block, offset, 1) << 4;       /* bz[4]   */
            r[1] |= bc6_read(block, offset, 6);        /* rx[5:0] */
            g[2] |= bc6_read(block, offset, 4);        /* gy[3:0] */
            g[1] |= bc6_read(block, offset, 5);        /* gx[4:0] */
            b[3] |= bc6_read(block, offset, 1);            /* bz[0]   */
            g[3] |= bc6_read(block, offset, 4);        /* gz[3:0] */
            b[1] |= bc6_read(block, offset, 5);        /* bx[4:0] */
            b[3] |= bc6_read(block, offset, 1) << 1;       /* bz[1]   */
            b[2] |= bc6_read(block, offset, 4);        /* by[3:0] */
            r[2] |= bc6_read(block, offset, 6);        /* ry[5:0] */
            r[3] |= bc6_read(block, offset, 6);        /* rz[5:0] */
            partition_id = bc6_read(block, offset, 5);    /* d[4:0]  */
            mode = 6;
        } break;

        /* mode 8 */
        case 22: {
            /* Partitition indices: 46 bits
               Partition: 5 bits
               Color Endpoints: 72 bits (8555, 8666, 8555) */
            r[0] |= bc6_read(block, offset, 8);        /* rw[7:0] */
            b[3] |= bc6_read(block, offset, 1);            /* bz[0]   */
            b[2] |= bc6_read(block, offset, 1) << 4;       /* by[4]   */
            g[0] |= bc6_read(block, offset, 8);        /* gw[7:0] */
            g[2] |= bc6_read(block, offset, 1) << 5;       /* gy[5]   */
            g[2] |= bc6_read(block, offset, 1) << 4;       /* gy[4]   */
            b[0] |= bc6_read(block, offset, 8);        /* bw[7:0] */
            g[3] |= bc6_read(block, offset, 1) << 5;       /* gz[5]   */
            b[3] |= bc6_read(block, offset, 1) << 4;       /* bz[4]   */
            r[1] |= bc6_read(block, offset, 5);        /* rx[4:0] */
            g[3] |= bc6_read(block, offset, 1) << 4;       /* gz[4]   */
            g[2] |= bc6_read(block, offset, 4);        /* gy[3:0] */
            g[1] |= bc6_read(block, offset, 6);        /* gx[5:0] */
            g[3] |= bc6_read(block, offset, 4);        /* zx[3:0] */
            b[1] |= bc6_read(block, offset, 5);        /* bx[4:0] */
            b[3] |= bc6_read(block, offset, 1) << 1;       /* bz[1]   */
            b[2] |= bc6_read(block, offset, 4);        /* by[3:0] */
            r[2] |= bc6_read(block, offset, 5);        /* ry[4:0] */
            b[3] |= bc6_read(block, offset, 1) << 2;       /* bz[2]   */
            r[3] |= bc6_read(block, offset, 5);        /* rz[4:0] */
            b[3] |= bc6_read(block, offset, 1) << 3;       /* bz[3]   */
            partition_id = bc6_read(block, offset, 5);    /* d[4:0]  */
            mode = 7;
        } break;

        /* mode 9 */
        case 26: {
            /* Partitition indices: 46 bits
               Partition: 5 bits
               Color Endpoints: 72 bits (8555, 8555, 8666) */
            r[0] |= bc6_read(block, offset, 8);        /* rw[7:0] */
            b[3] |= bc6_read(block, offset, 1) << 1;       /* bz[1]   */
            b[2] |= bc6_read(block, offset, 1) << 4;       /* by[4]   */
            g[0] |= bc6_read(block, offset, 8);        /* gw[7:0] */
            b[2] |= bc6_read(block, offset, 1) << 5;       /* by[5]   */
            g[2] |= bc6_read(block, offset, 1) << 4;       /* gy[4]   */
            b[0] |= bc6_read(block, offset, 8);        /* bw[7:0] */
            b[3] |= bc6_read(block, offset, 1) << 5;       /* bz[5]   */
            b[3] |= bc6_read(block, offset, 1) << 4;       /* bz[4]   */
            r[1] |= bc6_read(block, offset, 5);        /* bw[4:0] */
            g[3] |= bc6_read(block, offset, 1) << 4;       /* gz[4]   */
            g[2] |= bc6_read(block, offset, 4);        /* gy[3:0] */
            g[1] |= bc6_read(block, offset, 5);        /* gx[4:0] */
            b[3] |= bc6_read(block, offset, 1);            /* bz[0]   */
            g[3] |= bc6_read(block, offset, 4);        /* gz[3:0] */
            b[1] |= bc6_read(block, offset, 6);        /* bx[5:0] */
            b[2] |= bc6_read(block, offset, 4);        /* by[3:0] */
            r[2] |= bc6_read(block, offset, 5);        /* ry[4:0] */
            b[3] |= bc6_read(block, offset, 1) << 2;       /* bz[2]   */
            r[3] |= bc6_read(block, offset, 5);        /* rz[4:0] */
            b[3] |= bc6_read(block, offset, 1) << 3;       /* bz[3]   */
            partition_id = bc6_read(block, offset, 5);    /* d[4:0]  */
            mode = 8;
        } break;

        /* mode 10 */
        case 30: {
            /* Partitition indices: 46 bits
               Partition: 5 bits
               Color Endpoints: 72 bits (6666, 6666, 6666) */
            r[0] |= bc6_read(block, offset, 6);        /* rw[5:0] */
            g[3] |= bc6_read(block, offset, 1) << 4;       /* gz[4]   */
            b[3] |= bc6_read(block, offset, 1);            /* bz[0]   */
            b[3] |= bc6_read(block, offset, 1) << 1;       /* bz[1]   */
            b[2] |= bc6_read(block, offset, 1) << 4;       /* by[4]   */
            g[0] |= bc6_read(block, offset, 6);        /* gw[5:0] */
            g[2] |= bc6_read(block, offset, 1) << 5;       /* gy[5]   */
            b[2] |= bc6_read(block, offset, 1) << 5;       /* by[5]   */
            b[3] |= bc6_read(block, offset, 1) << 2;       /* bz[2]   */
            g[2] |= bc6_read(block, offset, 1) << 4;       /* gy[4]   */
            b[0] |= bc6_read(block, offset, 6);        /* bw[5:0] */
            g[3] |= bc6_read(block, offset, 1) << 5;       /* gz[5]   */
            b[3] |= bc6_read(block, offset, 1) << 3;       /* bz[3]   */
            b[3] |= bc6_read(block, offset, 1) << 5;       /* bz[5]   */
            b[3] |= bc6_read(block, offset, 1) << 4;       /* bz[4]   */
            r[1] |= bc6_read(block, offset, 6);        /* rx[5:0] */
            g[2] |= bc6_read(block, offset, 4);        /* gy[3:0] */
            g[1] |= bc6_read(block, offset, 6);        /* gx[5:0] */
            g[3] |= bc6_read(block, offset, 4);        /* gz[3:0] */
            b[1] |= bc6_read(block, offset, 6);        /* bx[5:0] */
            b[2] |= bc6_read(block, offset, 4);        /* by[3:0] */
            r[2] |= bc6_read(block, offset, 6);        /* ry[5:0] */
            r[3] |= bc6_read(block, offset, 6);        /* rz[5:0] */
            partition_id = bc6_read(block, offset, 5);    /* d[4:0]  */
            mode = 9;
        } break;

        /* mode 11 */
        case 3: {
            /* Partitition indices: 63 bits
               Partition: 0 bits
               Color Endpoints: 60 bits (10.10, 10.10, 10.10) */
            r[0] |= bc6_read(block, offset, 10);       /* rw[9:0] */
            g[0] |= bc6_read(block, offset, 10);       /* gw[9:0] */
            b[0] |= bc6_read(block, offset, 10);       /* bw[9:0] */
            r[1] |= bc6_read(block, offset, 10);       /* rx[9:0] */
            g[1] |= bc6_read(block, offset, 10);       /* gx[9:0] */
            b[1] |= bc6_read(block, offset, 10);       /* bx[9:0] */
            mode = 10;
        } break;

        /* mode 12 */
        case 7: {
            /* Partitition indices: 63 bits
               Partition: 0 bits
               Color Endpoints: 60 bits (11.9, 11.9, 11.9) */
            r[0] |= bc6_read(block, offset, 10);       /* rw[9:0] */
            g[0] |= bc6_read(block, offset, 10);       /* gw[9:0] */
            b[0] |= bc6_read(block, offset, 10);       /* bw[9:0] */
            r[1] |= bc6_read(block, offset, 9);        /* rx[8:0] */
            r[0] |= bc6_read(block, offset, 1) << 10;      /* rw[10]  */
            g[1] |= bc6_read(block, offset, 9);        /* gx[8:0] */
            g[0] |= bc6_read(block, offset, 1) << 10;      /* gw[10]  */
            b[1] |= bc6_read(block, offset, 9);        /* bx[8:0] */
            b[0] |= bc6_read(block, offset, 1) << 10;      /* bw[10]  */
            mode = 11;
        } break;

        /* mode 13 */
        case 11: {
            /* Partitition indices: 63 bits
               Partition: 0 bits
               Color Endpoints: 60 bits (12.8, 12.8, 12.8) */
            r[0] |= bc6_read(block, offset, 10);       /* rw[9:0] */
            g[0] |= bc6_read(block, offset, 10);       /* gw[9:0] */
            b[0] |= bc6_read(block, offset, 10);       /* bw[9:0] */
            r[1] |= bc6_read(block, offset, 8);        /* rx[7:0] */
            r[0] |= bc6_read_reverse(block, offset, 2) << 10;/* rx[10:11] */
            g[1] |= bc6_read(block, offset, 8);        /* gx[7:0] */
            g[0] |= bc6_read_reverse(block, offset, 2) << 10;/* gx[10:11] */
            b[1] |= bc6_read(block, offset, 8);        /* bx[7:0] */
            b[0] |= bc6_read_reverse(block, offset, 2) << 10;/* bx[10:11] */
            mode = 12;
        } break;

        /* mode 14 */
        case 15: {
            /* Partitition indices: 63 bits
               Partition: 0 bits
               Color Endpoints: 60 bits (16.4, 16.4, 16.4) */
            r[0] |= bc6_read(block, offset, 10);       /* rw[9:0] */
            g[0] |= bc6_read(block, offset, 10);       /* gw[9:0] */
            b[0] |= bc6_read(block, offset, 10);       /* bw[9:0] */
            r[1] |= bc6_read(block, offset, 4);        /* rx[3:0] */
            r[0] |= bc6_read_reverse(block, offset, 6) << 10;/* rw[10:15] */
            g[1] |= bc6_read(block, offset, 4);        /* gx[3:0] */
            g[0] |= bc6_read_reverse(block, offset, 6) << 10;/* gw[10:15] */
            b[1] |= bc6_read(block, offset, 4);        /* bx[3:0] */
            b[0] |= bc6_read_reverse(block, offset, 6) << 10;/* bw[10:15] */
            mode = 13;
        } break;

        default: return uvec2(0u, 0x3c000000u);
    }
    int endpoints = mode >= 10 ? 2 : 4;
    ivec4 bits = endpoint_bits[mode];
    if (signed_format) {
        r[0] = bc6_sign(r[0], bits.x);
        g[0] = bc6_sign(g[0], bits.x);
        b[0] = bc6_sign(b[0], bits.x);
    }
    for (int i = 1; i < endpoints; ++i) {
        if ((mode != 9 && mode != 10) || signed_format) {
            r[i] = bc6_sign(r[i], bits.y);
            g[i] = bc6_sign(g[i], bits.z);
            b[i] = bc6_sign(b[i], bits.w);
        }
        if (mode != 9 && mode != 10) {
            int mask = (1 << bits.x) - 1;
            r[i] = (r[i] + r[0]) & mask;
            g[i] = (g[i] + g[0]) & mask;
            b[i] = (b[i] + b[0]) & mask;
            if (signed_format) {
                r[i] = bc6_sign(r[i], bits.x);
                g[i] = bc6_sign(g[i], bits.x);
                b[i] = bc6_sign(b[i], bits.x);
            }
        }
    }
    uvec2 shape = mode >= 10 ? uvec2(0u, 1u) : bc7_partitions[partition_id];
    uint subset = (shape.x >> (2u * pixel)) & 1u;
    uint index_bits = mode >= 10 ? 4u : 3u;
    offset += pixel * index_bits - uint(bitCount(shape.y & ((1u << pixel) - 1u)));
    uint index = bc7_bits(block, offset, index_bits - ((shape.y >> pixel) & 1u));
    int weight = int(bc7_weight(index_bits, index));
    int a = int(2u * subset), z = a + 1;
    int red = (bc6_unquantize(r[a], bits.x, signed_format) * (64 - weight) +
               bc6_unquantize(r[z], bits.x, signed_format) * weight + 32) >> 6;
    int green = (bc6_unquantize(g[a], bits.x, signed_format) * (64 - weight) +
                 bc6_unquantize(g[z], bits.x, signed_format) * weight + 32) >> 6;
    int blue = (bc6_unquantize(b[a], bits.x, signed_format) * (64 - weight) +
                bc6_unquantize(b[z], bits.x, signed_format) * weight + 32) >> 6;
    return uvec2(bc6_finish(red, signed_format) | (bc6_finish(green, signed_format) << 16),
                 bc6_finish(blue, signed_format) | 0x3c000000u);
}
