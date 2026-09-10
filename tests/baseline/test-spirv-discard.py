#!/usr/bin/env python3
"""Validate discard rewriting for vertex, geometry and tessellation SPIR-V.
Requires a C compiler, Vulkan headers, glslangValidator and spirv-val.
"""
import ctypes as c, subprocess, tempfile, os
from pathlib import Path
root = Path(__file__).resolve().parents[2]
workspace = tempfile.TemporaryDirectory(prefix='hybris-discard-')
out = Path(workspace.name)
header = os.environ.get('VULKAN_INCLUDE', '/usr/include')
subprocess.run([os.environ.get('CC', 'cc'), '-shared', '-fPIC', '-O2', '-Wall', '-Wextra',
    '-I' + header, *(str(root / 'hybris/vulkan/compat' / name) for name in
    ('spirv_discard.c', 'spirv_entry.c', 'spirv_decorations.c')),
    '-o', str(out / 'discard.so')], check=True)
lib=c.CDLL(str(out / 'discard.so'));fn=lib.hybris_spirv_discard
fn.argtypes=[c.POINTER(c.c_uint32),c.c_size_t,c.c_uint32,c.c_char_p,c.c_void_p,c.POINTER(c.c_void_p),c.POINTER(c.c_size_t),c.POINTER(c.c_char_p)]
lib.hybris_spirv_storage_writes.argtypes=[c.POINTER(c.c_uint32),c.c_size_t,c.c_void_p,c.POINTER(c.c_int)]
free=c.CDLL(None).free;free.argtypes=[c.c_void_p]
fixtures={
'buffer.vert':(0,(root/'tests/baseline/shaders/vertex-store.vert').read_text()),
'no-position.vert':(0,'#version 450\nlayout(set=0,binding=0) buffer S {uint count;} s; void main(){atomicAdd(s.count,1u);}\n'),
'early.vert':(0,'#version 450\nvoid main(){gl_Position=vec4(1);if(gl_VertexIndex==0)return;gl_Position=vec4(0);}\n'),
'geometry.geom':(3,'#version 450\nlayout(points) in; layout(points,max_vertices=1) out; layout(set=0,binding=0) buffer S{uint count;}s; void main(){atomicAdd(s.count,1u);gl_Position=gl_in[0].gl_Position;EmitVertex();EndPrimitive();}\n'),
'eval.tese':(2,'#version 450\nlayout(triangles,equal_spacing,cw) in; layout(set=0,binding=0) buffer S{uint count;}s; void main(){atomicAdd(s.count,1u);gl_Position=vec4(gl_TessCoord,1);}\n'),
}
for env in ['vulkan1.0','vulkan1.2']:
 for name,(model,source) in fixtures.items():
  path=out/(env+'-'+name);path.write_text(source);spv=path.with_suffix(path.suffix+'.spv')
  subprocess.run(['glslangValidator','-V','--target-env',env,str(path),'-o',str(spv)],check=True,stdout=subprocess.DEVNULL)
  data=spv.read_bytes();words=(c.c_uint32*(len(data)//4)).from_buffer_copy(data)
  output=c.c_void_p();size=c.c_size_t();reason=c.c_char_p();writes=c.c_int()
  assert lib.hybris_spirv_storage_writes(words,len(data),None,c.byref(writes))==0
  assert writes.value == (name != 'early.vert'), (name, writes.value)
  rc=fn(words,len(data),model,b'main',None,c.byref(output),c.byref(size),c.byref(reason));assert rc==0,(name,rc,reason.value)
  dst=out/(env+'-'+name+'.discard.spv');dst.write_bytes(c.string_at(output,size.value));free(output)
  subprocess.run(['spirv-val','--target-env',env,str(dst)],check=True)
  print(env,name,'writes=',writes.value,'PASS')
