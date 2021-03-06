/* Copyright 2015-2017 Philippe Tillet
* 
* Permission is hereby granted, free of charge, to any person obtaining 
* a copy of this software and associated documentation files 
* (the "Software"), to deal in the Software without restriction, 
* including without limitation the rights to use, copy, modify, merge, 
* publish, distribute, sublicense, and/or sell copies of the Software, 
* and to permit persons to whom the Software is furnished to do so, 
* subject to the following conditions:
* 
* The above copyright notice and this permission notice shall be 
* included in all copies or substantial portions of the Software.
* 
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include <cstring>
#include <iostream>
#include "isaac/jit/syntax/engine/process.h"
#include "isaac/jit/generation/reduce_1d.h"
#include "isaac/jit/generation/engine/keywords.h"
#include "tools/loop.hpp"
#include "tools/reductions.hpp"
#include "tools/vector_types.hpp"
#include "tools/arguments.hpp"
#include <string>


namespace isaac
{
namespace templates
{

unsigned int reduce_1d::lmem_usage(expression_tree const  & x) const
{
  return ls0_*size_of(x.dtype());
}

unsigned int reduce_1d::temporary_workspace(expression_tree const &) const
{
    if(ng_ > 1)
      return ng_;
    return 0;
}

expression_type reduce_1d::type() const
{ return REDUCE_1D; }

inline void reduce_1d::reduce_1d_local_memory(kernel_generation_stream & stream, unsigned int size, std::vector<symbolic::reduce_1d*> exprs,
                                   std::string const & buf_str, std::string const & buf_value_str, driver::backend_type) const
{
  stream << "#pragma unroll" << std::endl;
  stream << "for(unsigned int stride = " << size/2 << "; stride > 0; stride /=2)" << std::endl;
  stream << "{" << std::endl;
  stream.inc_tab();
  stream << "$LOCAL_BARRIER;" << std::endl;
  stream << "if (lid <  stride)" << std::endl;
  stream << "{" << std::endl;
  stream.inc_tab();

  for (symbolic::reduce_1d* rd : exprs)
    if (is_indexing(rd->op().type))
      compute_index_reduce_1d(stream, rd->process(buf_str+"[lid]"), rd->process(buf_str+"[lid+stride]")
                              , rd->process(buf_value_str+"[lid]"), rd->process(buf_value_str+"[lid+stride]"),
                              rd->op());
    else
      compute_reduce_1d(stream, rd->process(buf_str+"[lid]"), rd->process(buf_str+"[lid+stride]"), rd->op());
  stream.dec_tab();
  stream << "}" << std::endl;
  stream.dec_tab();
  stream << "}" << std::endl;
}

std::string reduce_1d::generate_impl(std::string const & suffix, expression_tree const  & tree, driver::Device const & device, symbolic::symbols_table const & symbols) const
{
  kernel_generation_stream stream(device.backend());

  std::vector<symbolic::reduce_1d*> reductions = symbolic::extract<symbolic::reduce_1d>(tree, symbols);
  std::vector<std::size_t> assignments = symbolic::assignments(tree);

  driver::backend_type backend = device.backend();

  auto unroll_tmp = [&]()
  {
      unsigned int offset = 0;
      for(symbolic::reduce_1d* rd: reductions)
      {
        numeric_type dtype = tree.dtype();
        std::string sdtype = to_string(dtype);
        if (is_indexing(rd->op().type))
        {
          stream << rd->process("$GLOBAL uint* #name_temp = ($GLOBAL uint *)(tmp + " + tools::to_string(offset) + ");");
          offset += 4*ng_;
          stream << rd->process("$GLOBAL " + sdtype + "* #name_temp_value = ($GLOBAL " + sdtype + "*)(tmp + " + tools::to_string(offset) + ");");
          offset += size_of(dtype)*ng_;
        }
        else{
          stream << rd->process("$GLOBAL " + sdtype + "* #name_temp = ($GLOBAL " + sdtype + "*)(tmp + " + tools::to_string(offset) + ");");
          offset += size_of(dtype)*ng_;
        }
      }
  };

  /* ------------------------
   * Kernel 1
   * -----------------------*/
  switch(backend)
  {
    case driver::CUDA:
      stream << "#include  \"vector.h\"" << std::endl; break;
    case driver::OPENCL:
      if(tree.dtype()==HALF_TYPE)
        stream << "#pragma OPENCL EXTENSION cl_khr_fp16: enable" << std::endl;
      stream << " __attribute__((reqd_work_group_size(" << ls0_ << ",1,1)))" << std::endl; break;
  }
  stream << "$KERNEL void prod" << suffix << "($SIZE_T N, $GLOBAL char* tmp," << tools::join(kernel_arguments(device, symbols, tree), ", ") << ")" << std::endl;
  stream << "{" << std::endl;
  //Unroll
  stream.inc_tab();
  unroll_tmp();
  //Declare
  stream << "unsigned int lid = $LOCAL_IDX_0;" << std::endl;
  stream << "unsigned int gid = $GLOBAL_IDX_0;" << std::endl;
  stream << "unsigned int gpid = $GROUP_IDX_0;" << std::endl;
  stream << "unsigned int gsize = $GLOBAL_SIZE_0;" << std::endl;

  for(symbolic::reduce_1d* rd: reductions)
  {
    if(is_indexing(rd->op().type))
    {
      stream << rd->process("$LOCAL #scalartype #name_buf_value[" + tools::to_string(ls0_) + "];") << std::endl;
      stream << rd->process("#scalartype #name_acc_value = " + neutral_element(rd->op(), backend, "#scalartype") + ";") << std::endl;
      stream << rd->process("$LOCAL unsigned int #name_buf[" + tools::to_string(ls0_) + "];") << std::endl;
      stream << rd->process("unsigned int #name_acc = 0;") << std::endl;
    }
    else
    {
      stream << rd->process("$LOCAL #scalartype #name_buf[" + tools::to_string(ls0_) + "];") << std::endl;
      stream << rd->process("#scalartype #name_acc = " + neutral_element(rd->op(), backend, "#scalartype") + ";") << std::endl;
    }
  }
  stream << tools::join(negative_inc_process(device, symbols, tree), "  ") << std::endl;
  element_wise_loop_1D(stream, vwidth_, "i", "N", "$GLOBAL_IDX_0", "$GLOBAL_SIZE_0", [&](unsigned int vwidth)
  {
    std::string dtype = append_width("#scalartype",vwidth);
    //Fetch vector entry
    std::set<std::string> fetched;
     for (symbolic::reduce_1d* rd : reductions)
       for(symbolic::leaf* leaf: symbolic::extract<symbolic::leaf>(tree, symbols, rd->root(), false))
          if(fetched.insert(leaf->process("#name")).second)
            stream << leaf->process(dtype + " #name = " + append_width("loadv", vwidth) + "(i);") << std::endl;
    //Update accumulators
    for (symbolic::reduce_1d* rd : reductions)
      for (unsigned int s = 0; s < vwidth; ++s)
      {
        std::string value = rd->lhs()->evaluate({{"leaf", access_vector_type("#name", s, vwidth)}});
        if (is_indexing(rd->op().type))
          compute_index_reduce_1d(stream, rd->process("#name_acc"),  "i*" + tools::to_string(vwidth) + "+" + tools::to_string(s), rd->process("#name_acc_value"), value,rd->op());
        else
          compute_reduce_1d(stream, rd->process("#name_acc"), value,rd->op());
      }
  });
  //Fills local memory
  for(symbolic::reduce_1d* rd: reductions)
  {
    if (is_indexing(rd->op().type))
      stream << rd->process("#name_buf_value[lid] = #name_acc_value;") << std::endl;
    stream << rd->process("#name_buf[lid] = #name_acc;") << std::endl;
  }
  //Reduce local memory
  reduce_1d_local_memory(stream, ls0_, reductions, "#name_buf", "#name_buf_value", backend);
  //Write to temporary buffers
  stream << "if (lid==0)" << std::endl;
  stream << "{" << std::endl;
  stream.inc_tab();
  for(symbolic::reduce_1d* rd: reductions)
  {
    if (is_indexing(rd->op().type))
      stream << rd->process("#name_temp_value[gpid] = #name_buf_value[0];") << std::endl;
    stream << rd->process("#name_temp[gpid] = #name_buf[0];") << std::endl;
  }
  stream.dec_tab();
  stream << "}" << std::endl;
  stream.dec_tab();
  stream << "}" << std::endl;


  /* ------------------------
   * Kernel 2
   * -----------------------*/
  stream << "$KERNEL void reduce" << suffix << "($SIZE_T N, $GLOBAL char* tmp, " << tools::join(kernel_arguments(device, symbols, tree), ", ") << ")" << std::endl;
  stream << "{" << std::endl;
  stream.inc_tab();
  unroll_tmp();
  stream << tools::join(negative_inc_process(device, symbols, tree), "  ") << std::endl;
  //Declarations
  stream << "unsigned int lid = $LOCAL_IDX_0;" << std::endl;
  stream << "unsigned int lsize = $LOCAL_SIZE_0;" << std::endl;
  for (symbolic::reduce_1d* rd: reductions)
  {
    if (is_indexing(rd->op().type))
    {
      stream << rd->process("$LOCAL unsigned int #name_buf[" + tools::to_string(ls0_) + "];");
      stream << rd->process("unsigned int #name_acc = 0;") << std::endl;
      stream << rd->process("$LOCAL #scalartype #name_buf_value[" + tools::to_string(ls0_) + "];") << std::endl;
      stream << rd->process("#scalartype #name_acc_value = " + neutral_element(rd->op(), backend, "#scalartype") + ";");
    }
    else
    {
      stream << rd->process("$LOCAL #scalartype #name_buf[" + tools::to_string(ls0_) + "];") << std::endl;
      stream << rd->process("#scalartype #name_acc = " + neutral_element(rd->op(), backend, "#scalartype") + ";");      
    }
  }
  //Private reduction
  stream << "for(unsigned int i = lid; i < " << ng_ << "; i += lsize)" << std::endl;
  stream << "{" << std::endl;
  stream.inc_tab();
  for (symbolic::reduce_1d* rd: reductions)
    if (is_indexing(rd->op().type))
      compute_index_reduce_1d(stream, rd->process("#name_acc"), rd->process("#name_temp[i]"), rd->process("#name_acc_value"),rd->process("#name_temp_value[i]"),rd->op());
    else
      compute_reduce_1d(stream, rd->process("#name_acc"), rd->process("#name_temp[i]"), rd->op());
  stream.dec_tab();
  stream << "}" << std::endl;
  for(symbolic::reduce_1d* rd: reductions)
  {
    if (is_indexing(rd->op().type))
      stream << rd->process("#name_buf_value[lid] = #name_acc_value;") << std::endl;
    stream << rd->process("#name_buf[lid] = #name_acc;") << std::endl;
  }
  //Local reduction
  reduce_1d_local_memory(stream, ls0_, reductions, "#name_buf", "#name_buf_value", backend);
  //Write
  stream << "if (lid==0)" << std::endl;
  stream << "{" << std::endl;
  stream.inc_tab();
  for(size_t idx: assignments)
    stream << symbols.at(idx)->evaluate({{"reduce_1d", "#name_buf[0]"}, {"leaf", "at(0)"}}) << ";" << std::endl;
  stream.dec_tab();
  stream << "}" << std::endl;
  stream.dec_tab();
  stream << "}" << std::endl;
//  std::cout<<"reduce 1d: "<<stream.str()<<std::endl;

  return stream.str();
}

reduce_1d::reduce_1d(unsigned int vwidth, unsigned int ls, unsigned int ng):
    parameterized_base(vwidth,ls,1), ng_(ng)
{}

std::vector<int_t> reduce_1d::input_sizes(expression_tree const  & x) const
{
  std::vector<size_t> idx = symbolic::find(x, [](expression_tree::node const & x){return x.type==COMPOSITE_OPERATOR_TYPE && x.binary_operator.op.type_family==REDUCE;});
  size_t lhs = x[idx[0]].binary_operator.lhs;
  return {max(x[lhs].shape)};
}

void reduce_1d::enqueue(driver::CommandQueue & queue, driver::Program const & program, std::string const & suffix, runtime::execution_handler const & control)
{
  expression_tree const  & x = control.x();

  //Preprocessing
  int_t size = input_sizes(x)[0];

  //Kernel
  std::string name[2] = {"prod", "reduce"};
  name[0] += suffix;
  name[1] += suffix;

  driver::Kernel kernels[2] = { driver::Kernel(program,name[0].c_str()), driver::Kernel(program,name[1].c_str()) };

  //NDRange
  driver::NDRange global[2] = { driver::NDRange(ls0_*ng_), driver::NDRange(ls0_) };
  driver::NDRange local[2] = { driver::NDRange(ls0_), driver::NDRange(ls0_) };
  //Arguments
  for (auto & kernel : kernels)
  {
    unsigned int n_arg = 0;
    kernel.setSizeArg(n_arg++, size);
    kernel.setArg(n_arg++, driver::backend::workspaces::get(queue));
    symbolic::set_arguments(x, kernel, n_arg);
  }

  for (unsigned int k = 0; k < 2; k++)
    control.execution_options().enqueue(program.context(), kernels[k], global[k], local[k]);
  queue.synchronize();
}

}
}
