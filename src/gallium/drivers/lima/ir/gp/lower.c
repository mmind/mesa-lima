/*
 * Copyright (c) 2017 Lima Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include "util/ralloc.h"

#include "gpir.h"
#include "lima_context.h"

static bool gpir_lower_const(gpir_compiler *comp)
{
   int num_constant = 0;
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      list_for_each_entry_safe(gpir_node, node, &block->node_list, list) {
         if (node->op == gpir_op_const) {
            if (gpir_node_is_root(node))
               gpir_node_delete(node);
            else
               num_constant++;
         }
      }
   }

   if (num_constant) {
      union fi *constant = ralloc_array(comp->prog, union fi, num_constant);
      if (!constant)
         return false;

      comp->prog->constant = constant;
      comp->prog->constant_size = num_constant * sizeof(union fi);

      int index = 0;
      list_for_each_entry(gpir_block, block, &comp->block_list, list) {
         list_for_each_entry_safe(gpir_node, node, &block->node_list, list) {
            if (node->op == gpir_op_const) {
               gpir_const_node *c = gpir_node_to_const(node);

               if (!gpir_node_is_root(node)) {
                  gpir_load_node *load = gpir_node_create(block, gpir_op_load_uniform);
                  if (unlikely(!load))
                     return false;

                  load->index = comp->constant_base + (index >> 2);
                  load->component = index % 4;
                  constant[index++] = c->value;

                  gpir_node_replace_succ(&load->node, node);

                  list_addtail(&load->node.list, &node->list);

                  gpir_debug("lower const create uniform %d for const %d\n",
                             load->node.index, node->index);
               }

               gpir_node_delete(node);
            }
         }
      }
   }

   return true;
}

/* duplicate load to all its successors */
static bool gpir_lower_load(gpir_compiler *comp)
{
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      list_for_each_entry_safe(gpir_node, node, &block->node_list, list) {
         if (node->type == gpir_node_type_load) {
            gpir_load_node *load = gpir_node_to_load(node);

            bool first = true;
            gpir_node_foreach_succ_safe(node, dep) {
               gpir_node *succ = dep->succ;

               if (first) {
                  first = false;
                  continue;
               }

               gpir_node *new = gpir_node_create(succ->block, node->op);
               if (unlikely(!new))
                  return false;
               list_addtail(&new->list, &succ->list);

               gpir_debug("lower load create %d from %d for succ %d\n",
                          new->index, node->index, succ->index);

               gpir_load_node *nload = gpir_node_to_load(new);
               nload->index = load->index;
               nload->component = load->component;
               if (load->reg) {
                  nload->reg = load->reg;
                  list_addtail(&nload->reg_link, &load->reg->uses_list);
               }

               gpir_node_replace_pred(dep, new);
               gpir_node_replace_child(succ, node, new);
            }
         }
      }
   }

   return true;
}

static bool gpir_lower_neg(gpir_block *block, gpir_node *node)
{
   gpir_alu_node *neg = gpir_node_to_alu(node);
   gpir_node *child = neg->children[0];

   /* check if child can dest negate */
   if (child->type == gpir_node_type_alu) {
      /* negate must be its only successor */
      if (list_is_singular(&child->succ_list) &&
          gpir_op_infos[child->op].dest_neg) {
         gpir_alu_node *alu = gpir_node_to_alu(child);
         alu->dest_negate = !alu->dest_negate;

         gpir_node_replace_succ(child, node);
         gpir_node_delete(node);
         return true;
      }
   }

   /* check if child can src negate */
   gpir_node_foreach_succ_safe(node, dep) {
      gpir_node *succ = dep->succ;
      if (succ->type != gpir_node_type_alu)
         continue;

      bool success = true;
      gpir_alu_node *alu = gpir_node_to_alu(dep->succ);
      for (int i = 0; i < alu->num_child; i++) {
         if (alu->children[i] == node) {
            if (gpir_op_infos[succ->op].src_neg[i]) {
               alu->children_negate[i] = !alu->children_negate[i];
               alu->children[i] = child;
            }
            else
               success = false;
         }
      }

      if (success)
         gpir_node_replace_pred(dep, child);
   }

   if (gpir_node_is_root(node))
      gpir_node_delete(node);

   return true;
}

static bool gpir_lower_complex(gpir_block *block, gpir_node *node)
{
   gpir_alu_node *alu = gpir_node_to_alu(node);
   gpir_node *child = alu->children[0];

   gpir_alu_node *complex2 = gpir_node_create(block, gpir_op_complex2);
   if (unlikely(!complex2))
      return false;

   complex2->children[0] = child;
   complex2->num_child = 1;
   gpir_node_add_dep(&complex2->node, child, GPIR_DEP_INPUT);
   list_addtail(&complex2->node.list, &node->list);

   int impl_op = 0;
   switch (node->op) {
   case gpir_op_rcp:
      impl_op = gpir_op_rcp_impl;
      break;
   case gpir_op_rsqrt:
      impl_op = gpir_op_rsqrt_impl;
      break;
   default:
      assert(0);
   }

   gpir_alu_node *impl = gpir_node_create(block, impl_op);
   if (unlikely(!impl))
      return false;

   impl->children[0] = child;
   impl->num_child = 1;
   gpir_node_add_dep(&impl->node, child, GPIR_DEP_INPUT);
   list_addtail(&impl->node.list, &node->list);

   /* change node to complex1 node */
   node->op = gpir_op_complex1;
   alu->children[0] = &impl->node;
   alu->children[1] = &complex2->node;
   alu->children[2] = child;
   alu->num_child = 3;
   gpir_node_add_dep(node, &impl->node, GPIR_DEP_INPUT);
   gpir_node_add_dep(node, &complex2->node, GPIR_DEP_INPUT);

   return true;
}

static bool gpir_lower_node_may_consume_two_slots(gpir_compiler *comp)
{
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      list_for_each_entry_safe(gpir_node, node, &block->node_list, list) {
         if (gpir_op_infos[node->op].may_consume_two_slots) {
            /* dummy_f/m are auxiliary nodes for value reg alloc:
             * 1. before reg alloc, create fake nodes dummy_f, dummy_m,
             *    so the tree become: (dummy_m (node dummy_f))
             *    dummy_m can be spilled, but other nodes in the tree can't
             *    be spilled.
             * 2. After reg allocation and fake dep add, merge all deps of
             *    dummy_m and dummy_f to node and remove dummy_m & dummy_f
             *
             * We may also not use dummy_f/m, but alloc two value reg for
             * node. But that means we need to make sure there're 2 free
             * slot after the node successors, but we just need one slot
             * after to be able to schedule it because we can use one move for
             * the two slot node. It's also not easy to handle the spill case
             * for the alloc 2 value method.
             *
             * With the dummy_f/m method, there's no such requirement, the
             * node can be scheduled only when there's two slots for it,
             * otherwise a move. And the node can be spilled with one reg.
             */
            gpir_node *dummy_m = gpir_node_create(block, gpir_op_dummy_m);
            if (unlikely(!dummy_m))
               return false;
            list_add(&dummy_m->list, &node->list);

            gpir_node *dummy_f = gpir_node_create(block, gpir_op_dummy_f);
            if (unlikely(!dummy_f))
               return false;
            list_add(&dummy_f->list, &node->list);

            gpir_alu_node *alu = gpir_node_to_alu(dummy_m);
            alu->children[0] = node;
            alu->children[1] = dummy_f;
            alu->num_child = 2;

            gpir_node_replace_succ(dummy_m, node);
            gpir_node_add_dep(dummy_m, node, GPIR_DEP_INPUT);
            gpir_node_add_dep(dummy_m, dummy_f, GPIR_DEP_INPUT);

         }
      }
   }

   return true;
}

static bool (*gpir_lower_funcs[gpir_op_num])(gpir_block *, gpir_node *) = {
   [gpir_op_neg] = gpir_lower_neg,
   [gpir_op_rcp] = gpir_lower_complex,
   [gpir_op_rsqrt] = gpir_lower_complex,
};

bool gpir_pre_rsched_lower_prog(gpir_compiler *comp)
{
   if (!gpir_lower_const(comp))
      return false;

   if (!gpir_lower_load(comp))
      return false;

   gpir_debug("pre rsched lower prog\n");
   gpir_node_print_prog_seq(comp);
   return true;
}

bool gpir_post_rsched_lower_prog(gpir_compiler *comp)
{
   list_for_each_entry(gpir_block, block, &comp->block_list, list) {
      list_for_each_entry_safe(gpir_node, node, &block->node_list, list) {
         if (gpir_lower_funcs[node->op] &&
             !gpir_lower_funcs[node->op](block, node))
            return false;
      }
   }

   if (!gpir_lower_node_may_consume_two_slots(comp))
      return false;

   gpir_debug("post rsched lower prog\n");
   gpir_node_print_prog_seq(comp);
   return true;
}
