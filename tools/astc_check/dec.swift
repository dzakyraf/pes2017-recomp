import Metal
import Foundation
let blocksAcross = 256, blocksDown = 256
let data = try! Data(contentsOf: URL(fileURLWithPath: "blocks.astc"))
let dev = MTLCreateSystemDefaultDevice()!
let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .astc_4x4_ldr, width: blocksAcross*4, height: blocksDown*4, mipmapped: false)
td.usage = .shaderRead
let tex = dev.makeTexture(descriptor: td)!
data.withUnsafeBytes { p in
  tex.replace(region: MTLRegionMake2D(0,0,blocksAcross*4,blocksDown*4), mipmapLevel: 0, withBytes: p.baseAddress!, bytesPerRow: blocksAcross*16)
}
let src = """
#include <metal_stdlib>
using namespace metal;
kernel void k(texture2d<float, access::read> t [[texture(0)]], device uchar4* out [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
  float4 v = t.read(gid);
  out[gid.y * t.get_width() + gid.x] = uchar4(round(clamp(v, 0.0, 1.0) * 255.0));
}
"""
let lib = try! dev.makeLibrary(source: src, options: nil)
let pso = try! dev.makeComputePipelineState(function: lib.makeFunction(name: "k")!)
let w = blocksAcross*4, h = blocksDown*4
let buf = dev.makeBuffer(length: w*h*4, options: .storageModeShared)!
let q = dev.makeCommandQueue()!
let cb = q.makeCommandBuffer()!
let e = cb.makeComputeCommandEncoder()!
e.setComputePipelineState(pso); e.setTexture(tex, index: 0); e.setBuffer(buf, offset: 0, index: 0)
e.dispatchThreads(MTLSizeMake(w,h,1), threadsPerThreadgroup: MTLSizeMake(8,8,1))
e.endEncoding(); cb.commit(); cb.waitUntilCompleted()
try! Data(bytes: buf.contents(), count: w*h*4).write(to: URL(fileURLWithPath: "decoded.rgba"))
print("ok", dev.name)
