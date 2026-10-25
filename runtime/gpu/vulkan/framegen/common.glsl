// Shared by the frame generation passes (framegen.cpp). One descriptor set layout for all of them:
// bindings 0-2 sampled images (linear, clamped), binding 4 the pass's output.
layout(local_size_x = 8, local_size_y = 8) in;
