!!ARBvp1.0
# n64gfx vertex program. Vertices are the draw stream's own (OutVertex: clip-space position, the
# texture coordinate in texels, the shade); what a draw shares comes in constants:
#   c[0], c[1]: texture 0 / 1 mapping (u = s * x + y, v = t * z + w);  c[2]: primitive color;
#   c[3]: environment color;  c[4]: prim LOD fraction, fog color;  c[5]: alpha threshold, K5, fog alpha;
#   c[6]: position scale (x: widescreen widening);  c[8]: 3D: (depth d, -k d, -d convergence, eye sign s):
#   x += s * max(d * (w - convergence), -k * d * w): things at the convergence distance stay where they are,
#   farther ones move up to d (in -1..1 units) towards the eye's side, nearer ones the other way, by at most
#   k times d (all 0 in 2D, and for rectangles: at the screen's depth);  c[7]: unused (v80-v82 sent it out in texcoord5, which
#   the RSX didn't pass on to the fragment program as set up: the self-test read (0, 1, 0, 1)).
# Out: color: shade; texcoord0/1: texture 0/1 (+ fog alpha in 0.z; alpha threshold in 1.z, K5 in 1.w);
#   texcoord2: primitive color;  texcoord3: environment color;  texcoord4: prim LOD fraction, fog color.
TEMP R0, R1, R2, R3;
MOV R2, vertex.position;
MAD R3.x, R2.w, c[8].x, c[8].z;
MUL R3.y, R2.w, c[8].y;
MAX R3.x, R3.x, R3.y;
MAD R2.x, R3.x, c[8].w, R2.x;
MUL result.position, R2, c[6];
MOV result.color, vertex.color;
MAD R0.x, vertex.texcoord[0].x, c[0].x, c[0].y;
MAD R0.y, vertex.texcoord[0].y, c[0].z, c[0].w;
MOV R0.zw, c[5].zzzz;
MOV result.texcoord[0], R0;
MAD R1.x, vertex.texcoord[0].x, c[1].x, c[1].y;
MAD R1.y, vertex.texcoord[0].y, c[1].z, c[1].w;
MOV R1.zw, c[5].xxxy;
MOV result.texcoord[1], R1;
MOV result.texcoord[2], c[2];
MOV result.texcoord[3], c[3];
MOV result.texcoord[4], c[4];
END
