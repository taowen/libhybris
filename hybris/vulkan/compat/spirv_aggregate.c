/* SPDX-License-Identifier: Apache-2.0 */
#include "spirv_aggregate.h"
#include "spirv_entry.h"
#include "spirv_constants.h"
#include <string.h>

/* Integer matrices are not legal SPIR-V. Split affected aggregate inputs into
 * float scalar/vector leaves, then reconstruct the original float aggregate in
 * Private storage at entry. The scaled pass converts only matching leaves;
 * other columns retain their native fetch format and all value operations keep
 * their original types, including dynamic indexing and whole-aggregate loads.
 */
struct aggregate_id {
    uint32_t op, type, element, count, storage, location, component, decorations;
    uint32_t private_pointer, input_pointer, root;
    unsigned interface, component_set, resolved;
};
struct shape { size_t leaves, nodes, locations; unsigned depth, floating; };
struct node {
    uint32_t root, type, parent, index, index_id, input, loaded, address, location;
    uint32_t fetched;
    unsigned depth;
};
struct lowering {
    struct aggregate_id *ids;
    struct hybris_spirv_constants *constants;
    const uint32_t *code;
    size_t size;
    const VkSpecializationInfo *specialization;
    const VkAllocationCallbacks *allocator;
    VkResult constant_status;
    struct node *nodes;
    size_t used;
    uint32_t bound, next, integer;
    uint32_t fetch_scalar, fetch_type, fetch_pointer;
};

static uint32_t fresh(struct lowering *l)
{
    return l->next == UINT32_MAX ? 0 : l->next++;
}
static int shape_of(struct lowering *l, uint32_t type, unsigned depth, struct shape *shape)
{
    struct aggregate_id *ids = l->ids;
    uint32_t bound = l->bound;
    if (!type || type >= bound || depth > 64) return 0;
    const struct aggregate_id *id = &ids[type];
    if (id->op == 24 || id->op == 28) {
        uint32_t count = id->count;
        if (id->op == 28) {
            if (!count || count >= bound) return 0;
            struct aggregate_id *length = &ids[count];
            if (length->op == 43) {
                if (length->type >= bound || ids[length->type].op != 21 ||
                    ids[length->type].count != 32 ||
                    (ids[length->type].storage && (int32_t)length->count <= 0)) return 0;
            } else if (!length->resolved) {
                if (!l->constants) {
                    l->constant_status = hybris_spirv_constants_create(l->code, l->size,
                        l->specialization, l->allocator, &l->constants);
                    if (l->constant_status != VK_SUCCESS) return 0;
                }
                int sign;
                if (!hybris_spirv_constant_u32(l->constants, count, &length->count, &sign) ||
                    (sign && (int32_t)length->count <= 0)) return 0;
                length->resolved = 1;
            }
            count = length->count;
        }
        struct shape child;
        if (!count || !shape_of(l, id->element, depth + 1, &child) ||
            child.nodes > (SIZE_MAX - 1) / count || child.leaves > SIZE_MAX / count ||
            child.locations > SIZE_MAX / count) return 0;
        *shape = (struct shape){child.leaves * count, child.nodes * count + 1,
                               child.locations * count, child.depth + 1, child.floating};
        return 1;
    }
    uint32_t scalar = type, components = 1;
    if (id->op == 23) { scalar = id->element; components = id->count; }
    if (!scalar || scalar >= bound || !components || components > 4 ||
        (ids[scalar].op != 21 && ids[scalar].op != 22)) return 0;
    unsigned width = ids[scalar].count;
    if (width != 16 && width != 32 && width != 64) return 0;
    *shape = (struct shape){1, 1, width == 64 && components > 2 ? 2 : 1,
                           0, ids[scalar].op == 22 && width == 32};
    return 1;
}
static uint32_t pointer_type(struct lowering *l, uint32_t type, int input)
{
    uint32_t *slot = input ? &l->ids[type].input_pointer : &l->ids[type].private_pointer;
    if (!*slot) *slot = fresh(l);
    return *slot;
}
static int make_nodes(struct lowering *l, uint32_t root, uint32_t type, uint32_t parent,
                      uint32_t index, unsigned depth, uint32_t *location)
{
    if (l->used >= UINT32_MAX) return 0;
    uint32_t current = (uint32_t)l->used++;
    struct node *node = &l->nodes[current];
    *node = (struct node){.root = root, .type = type, .parent = parent, .index = index, .depth = depth};
    if (depth && !(node->index_id = fresh(l))) return 0;
    const struct aggregate_id *id = &l->ids[type];
    if (id->op == 24 || id->op == 28) {
        uint32_t count = id->op == 28 ? l->ids[id->count].count : id->count;
        for (uint32_t i = 0; i < count; ++i)
            if (!make_nodes(l, root, id->element, current, i, depth + 1, location)) return 0;
        return 1;
    }
    node->location = (*location)++;
    node->input = fresh(l); node->loaded = fresh(l); node->address = fresh(l);
    return node->input && node->loaded && node->address && pointer_type(l, type, 1) && pointer_type(l, type, 0);
}
static void initialize_inputs(struct lowering *l, uint32_t *result, size_t *out)
{
    for (size_t i = 0; i < l->used; ++i) {
        const struct node *node = &l->nodes[i];
        if (!node->input) continue;
        result[(*out)++] = (4u << 16) | 61;
        result[(*out)++] = node->fetched ? l->fetch_type : node->type;
        result[(*out)++] = node->fetched ? node->fetched : node->loaded;
        result[(*out)++] = node->input;
        if (node->fetched) {
            const struct aggregate_id *type = &l->ids[node->type];
            if (type->op == 22) {
                result[(*out)++] = (5u << 16) | 81; /* OpCompositeExtract */
                result[(*out)++] = node->type; result[(*out)++] = node->loaded;
                result[(*out)++] = node->fetched; result[(*out)++] = 2;
            } else {
                static const uint32_t components[] = {2, 1, 0, 3};
                result[(*out)++] = ((5u + type->count) << 16) | 79; /* OpVectorShuffle */
                result[(*out)++] = node->type; result[(*out)++] = node->loaded;
                result[(*out)++] = node->fetched; result[(*out)++] = node->fetched;
                for (uint32_t c = 0; c < type->count; ++c) result[(*out)++] = components[c];
            }
        }
        result[(*out)++] = ((4u + node->depth) << 16) | 65;
        result[(*out)++] = l->ids[node->type].private_pointer;
        result[(*out)++] = node->address; result[(*out)++] = node->root;
        size_t indices = *out;
        *out += node->depth;
        for (const struct node *at = node; at->depth; at = &l->nodes[at->parent])
            result[indices + at->depth - 1] = at->index_id;
        result[(*out)++] = (3u << 16) | 62;
        result[(*out)++] = node->address; result[(*out)++] = node->loaded;
    }
}

VkResult hybris_spirv_aggregate(const uint32_t *code, size_t size, const char *entry,
    const struct hybris_scaled_attribute *attributes, uint32_t attribute_count,
    const VkSpecializationInfo *specialization,
    const VkAllocationCallbacks *allocator, uint32_t **output, size_t *output_size,
    const char **reason)
{
    *output = NULL; *output_size = 0;
    *reason = "unsupported aggregate vertex input";
    if (size < 20 || size % 4 || code[0] != 0x07230203 || !code[3]) return VK_ERROR_UNKNOWN;
    struct lowering l = {.bound = code[3], .next = code[3], .code = code, .size = size,
        .specialization = specialization, .allocator = allocator, .constant_status = VK_SUCCESS};
    if (sizeof(*l.ids) > SIZE_MAX / l.bound) return VK_ERROR_OUT_OF_HOST_MEMORY;
    l.ids = hybris_scaled_alloc(allocator, l.bound * sizeof(*l.ids), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!l.ids) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(l.ids, 0, l.bound * sizeof(*l.ids));
    for (uint32_t i = 0; i < l.bound; ++i) l.ids[i].location = UINT32_MAX;
    VkResult status = VK_ERROR_UNKNOWN;
    uint32_t *result = NULL, selected_function = 0;
    size_t words = size / 4, selected_entry = 0, first_type = 0, first_function = 0;
    size_t node_count = 0, leaf_count = 0, path_words = 0;
    for (size_t at = 5; at < words;) {
        const uint32_t *p = code + at;
        uint32_t count = p[0] >> 16, op = p[0] & 0xffff;
        if (!count || count > words - at) goto done;
        if (op == 15) {
            if (count < 4 || p[2] >= l.bound) goto done;
            const char *name = (const char *)(p + 3), *end = memchr(name, 0, (count - 3) * 4);
            if (!end) goto done;
            if (!p[1] && !strcmp(name, entry)) {
                if (selected_entry) goto done;
                selected_entry = at; selected_function = p[2];
                for (size_t i = 3 + ((size_t)(end - name) + 4) / 4; i < count; ++i) {
                    if (p[i] >= l.bound) goto done;
                    l.ids[p[i]].interface = 1;
                }
            }
        } else if (op == 71) {
            if (count < 3 || p[1] >= l.bound) goto done;
            if (p[2] == 30 || p[2] == 31) {
                if (count != 4) goto done;
                if (p[2] == 30) l.ids[p[1]].location = p[3];
                else { l.ids[p[1]].component = p[3]; l.ids[p[1]].component_set = 1; }
            } else if (p[2] < 32) l.ids[p[1]].decorations |= 1u << p[2];
        } else if (op == 21 || op == 22 || op == 23 || op == 24 || op == 28 || op == 32) {
            if (count < 3 || p[1] >= l.bound) goto done;
            struct aggregate_id *id = &l.ids[p[1]];
            id->op = op;
            if (op == 21 || op == 22) { id->count = p[2]; if (op == 21) { if (count != 4) goto done; id->storage = p[3]; } }
            else {
                if (count != 4) goto done;
                if (op == 32) { id->storage = p[2]; id->element = p[3]; }
                else { id->element = p[2]; id->count = p[3]; }
            }
        } else if (op == 59 || op == 43 || op == 50 || op == 52) {
            if (count < 4 || p[1] >= l.bound || p[2] >= l.bound) goto done;
            l.ids[p[2]].op = op; l.ids[p[2]].type = p[1];
            if (op == 59) l.ids[p[2]].storage = p[3];
            else l.ids[p[2]].count = p[3];
        }
        if (op == 54 && (count != 5 || p[2] >= l.bound)) goto done;
        if (!first_type && op >= 19 && op <= 39) first_type = at;
        if (!first_function && op == 54) first_function = at;
        at += count;
    }
    if (!selected_entry || !first_type || !first_function) goto done;
    for (uint32_t i = 1; i < l.bound; ++i) {
        struct aggregate_id *id = &l.ids[i];
        if (id->op == 32 && id->element < l.bound) {
            if (id->storage == 1) l.ids[id->element].input_pointer = i;
            if (id->storage == 6) l.ids[id->element].private_pointer = i;
        }
        if (id->op == 21 && id->count == 32 && !id->storage) l.integer = i;
    }
    for (uint32_t i = 1; i < l.bound; ++i) {
        struct aggregate_id *id = &l.ids[i];
        if (id->op != 59 || id->storage != 1 || !id->interface || id->location == UINT32_MAX) continue;
        uint32_t pointer = id->type, type = l.ids[pointer].element;
        if (type >= l.bound) goto done;
        int scalar_vector = l.ids[type].op == 21 || l.ids[type].op == 22 || l.ids[type].op == 23;
        struct shape shape;
        if (!shape_of(&l, type, 0, &shape)) {
            if (l.constant_status != VK_SUCCESS) status = l.constant_status;
            goto done;
        }
        if (shape.locations > UINT32_MAX - id->location) goto done;
        int affected = 0;
        for (uint32_t j = 0; j < attribute_count; ++j)
            affected |= (!scalar_vector || attributes[j].rb_swizzle) &&
                attributes[j].location >= id->location && attributes[j].location - id->location < shape.locations;
        if (!affected) continue;
        if (!shape.floating || shape.nodes > SIZE_MAX - node_count || shape.leaves > SIZE_MAX - leaf_count ||
            shape.leaves > SIZE_MAX / (shape.depth + 1) || shape.leaves * shape.depth > SIZE_MAX - path_words) goto done;
        node_count += shape.nodes; leaf_count += shape.leaves; path_words += shape.leaves * shape.depth;
        id->root = i;
    }
    if (!leaf_count) { status = VK_SUCCESS; *reason = NULL; goto done; }
    if (node_count > UINT32_MAX || node_count > SIZE_MAX / sizeof(*l.nodes)) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    l.nodes = hybris_scaled_alloc(allocator, node_count * sizeof(*l.nodes), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!l.nodes) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    if (!l.integer && !(l.integer = fresh(&l))) goto done;
    for (uint32_t i = 1; i < l.bound; ++i) if (l.ids[i].root) {
        uint32_t location = l.ids[i].location, type = l.ids[l.ids[i].type].element;
        if (!pointer_type(&l, type, 0) || !make_nodes(&l, i, type, 0, 0, 0, &location)) goto done;
    }
    for (size_t i = 0; i < l.used; ++i) {
        struct node *node = &l.nodes[i];
        if (!node->input) continue;
        for (uint32_t j = 0; j < attribute_count; ++j) {
            if (!attributes[j].rb_swizzle || attributes[j].location != node->location) continue;
            if (l.ids[node->root].component) {
                *reason = "packed vertex swizzle does not support split Component inputs";
                goto done;
            }
            if (!(node->fetched = fresh(&l))) goto done;
            if (!l.fetch_scalar) {
                l.fetch_scalar = l.ids[node->type].op == 23 ? l.ids[node->type].element : node->type;
                for (uint32_t t = 1; t < l.bound; ++t)
                    if (l.ids[t].op == 23 && l.ids[t].element == l.fetch_scalar && l.ids[t].count == 4)
                        l.fetch_type = t;
                if (!l.fetch_type) {
                    l.fetch_type = fresh(&l);
                    l.fetch_pointer = fresh(&l);
                } else l.fetch_pointer = pointer_type(&l, l.fetch_type, 1);
                if (!l.fetch_type || !l.fetch_pointer) goto done;
            }
        }
    }
    uint32_t function = 0;
    int first_block = 0;
    size_t insertion = 0;
    for (size_t at = first_function; at < words; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        uint32_t count = p[0] >> 16, op = p[0] & 0xffff;
        if (op == 54) function = p[2];
        if (op == 56) function = 0;
        if (function == selected_function && op == 248) {
            if (!first_block) { first_block = 1; insertion = at + count; }
            else first_block = 2;
        }
        if (function == selected_function && first_block == 1 && op == 59) insertion = at + count;
        if (op == 65 || op == 66 || op == 83) {
            if (count < 4 || p[1] >= l.bound || p[2] >= l.bound || p[3] >= l.bound) goto done;
            if (l.ids[p[3]].root) {
                uint32_t type = l.ids[p[1]].element;
                if (l.ids[p[1]].op != 32 || l.ids[p[1]].storage != 1 || type >= l.bound || !pointer_type(&l, type, 0)) goto done;
                l.ids[p[2]].root = l.ids[p[3]].root;
            }
        } else if (count > 1 && !hybris_spirv_literal_word(op, 1) && p[1] < l.bound &&
                   l.ids[p[1]].op == 32 && l.ids[p[1]].storage == 1) {
            *reason = "unsupported aggregate Input pointer producer";
            goto done;
        }
        for (uint32_t i = 1; i < count; ++i) {
            if (hybris_spirv_literal_word(op, i) || p[i] >= l.bound || !l.ids[p[i]].root) continue;
            if ((op == 61 && i == 3) || ((op == 65 || op == 66 || op == 83) && (i == 2 || i == 3))) continue;
            *reason = "unsupported aggregate Input pointer use";
            goto done;
        }
    }
    if (!insertion) goto done;
    for (size_t at = 5; at < first_function; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        uint32_t op = p[0] & 0xffff, count = p[0] >> 16;
        if (op != 71 && op != 332 && op != 5632) continue;
        if (count < 3 || p[1] >= l.bound) goto done;
        if (l.ids[p[1]].root && (op != 71 ||
            (p[2] != 0 && p[2] != 13 && p[2] != 14 && p[2] != 16 && p[2] != 17 && p[2] != 30 && p[2] != 31))) {
            *reason = "unsupported aggregate Input decoration";
            goto done;
        }
    }
    size_t new_types = l.next - l.bound;
    if (leaf_count > SIZE_MAX / 64 || node_count > SIZE_MAX / 4 || new_types > SIZE_MAX / 4) {
        status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done;
    }
    size_t capacity = words;
    size_t additions[] = {leaf_count * 64, path_words, node_count * 4, new_types * 4, 16};
    for (unsigned i = 0; i < sizeof(additions) / sizeof(additions[0]); ++i) {
        if (additions[i] > SIZE_MAX - capacity) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
        capacity += additions[i];
    }
    if (capacity > SIZE_MAX / 4) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    result = hybris_scaled_alloc(allocator, capacity * 4, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (!result) { status = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    memcpy(result, code, 20);
    size_t out = 5;
    for (size_t at = 5; at < words; at += code[at] >> 16) {
        const uint32_t *p = code + at;
        uint32_t count = p[0] >> 16, op = p[0] & 0xffff;
        if (at == first_type) for (size_t i = 0; i < l.used; ++i) {
            const struct node *node = &l.nodes[i];
            if (!node->input) continue;
            const struct aggregate_id *root = &l.ids[node->root];
            result[out++] = (4u << 16) | 71; result[out++] = node->input; result[out++] = 30; result[out++] = node->location;
            if (root->component_set) {
                result[out++] = (4u << 16) | 71; result[out++] = node->input; result[out++] = 31; result[out++] = root->component;
            }
            for (unsigned d = 0; d < 32; ++d) if (root->decorations & (1u << d)) {
                if (d != 0 && d != 13 && d != 14 && d != 16 && d != 17) goto done;
                result[out++] = (3u << 16) | 71; result[out++] = node->input; result[out++] = d;
            }
        }
        if (at == selected_entry) {
            size_t start = out;
            size_t names = 3 + (strlen((const char *)(p + 3)) + 4) / 4;
            memcpy(result + out, p, names * 4); out += names;
            for (size_t i = names; i < count; ++i)
                if (!l.ids[p[i]].root || code[1] >= 0x00010400) result[out++] = p[i];
            for (size_t i = 0; i < l.used; ++i) if (l.nodes[i].input) result[out++] = l.nodes[i].input;
            if (out - start > 0xffff) goto done;
            result[start] = ((uint32_t)(out - start) << 16) | 15;
            continue;
        }
        if (op == 71 && ((l.ids[p[1]].root && p[2] != 0) ||
                        (l.ids[p[1]].resolved && p[2] == 1))) continue;
        /* Freeze only array-length result IDs used by this lowering. Other
         * specialization constants still reach the driver with the original
         * stage map, including dependencies used elsewhere in the shader. */
        if ((op == 50 || op == 52) && l.ids[p[2]].resolved) {
            result[out++] = (4u << 16) | 43; result[out++] = p[1];
            result[out++] = p[2]; result[out++] = l.ids[p[2]].count;
            continue;
        }
        if (at < first_function && op == 59) continue;
        if (at == first_function) {
            if (l.fetch_type >= l.bound) {
                result[out++] = (4u << 16) | 23; result[out++] = l.fetch_type;
                result[out++] = l.fetch_scalar; result[out++] = 4;
                result[out++] = (4u << 16) | 32; result[out++] = l.fetch_pointer;
                result[out++] = 1; result[out++] = l.fetch_type;
            }
            if (l.integer >= l.bound) {
                result[out++] = (4u << 16) | 21; result[out++] = l.integer; result[out++] = 32; result[out++] = 0;
            }
            for (uint32_t i = 1; i < l.bound; ++i) {
                uint32_t pointers[] = {l.ids[i].private_pointer, l.ids[i].input_pointer};
                for (unsigned j = 0; j < 2; ++j) if (pointers[j] >= l.bound) {
                    result[out++] = (4u << 16) | 32; result[out++] = pointers[j]; result[out++] = j ? 1 : 6; result[out++] = i;
                }
            }
            for (size_t i = 0; i < l.used; ++i) if (l.nodes[i].index_id) {
                result[out++] = (4u << 16) | 43; result[out++] = l.integer;
                result[out++] = l.nodes[i].index_id; result[out++] = l.nodes[i].index;
            }
            for (size_t declaration = 5; declaration < first_function; declaration += code[declaration] >> 16) {
                const uint32_t *v = code + declaration;
                if ((v[0] & 0xffff) != 59) continue;
                uint32_t n = v[0] >> 16;
                memcpy(result + out, v, n * 4);
                if (l.ids[v[2]].root) {
                    result[out + 1] = l.ids[l.ids[v[1]].element].private_pointer; result[out + 3] = 6;
                }
                out += n;
            }
            for (size_t i = 0; i < l.used; ++i) if (l.nodes[i].input) {
                result[out++] = (4u << 16) | 59;
                result[out++] = l.nodes[i].fetched ? l.fetch_pointer : l.ids[l.nodes[i].type].input_pointer;
                result[out++] = l.nodes[i].input; result[out++] = 1;
            }
        }
        if (at == insertion) initialize_inputs(&l, result, &out);
        memcpy(result + out, p, count * 4);
        if ((op == 65 || op == 66 || op == 83) && l.ids[p[2]].root)
            result[out + 1] = l.ids[l.ids[p[1]].element].private_pointer;
        out += count;
    }
    result[3] = l.next;
    *output = result; *output_size = out * 4; result = NULL;
    status = VK_SUCCESS; *reason = NULL;
done:
    hybris_scaled_free(allocator, result);
    hybris_scaled_free(allocator, l.nodes);
    hybris_scaled_free(allocator, l.ids);
    hybris_spirv_constants_destroy(l.constants, allocator);
    return status;
}
