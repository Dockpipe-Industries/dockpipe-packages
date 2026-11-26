# Original synthetic H264 fixture

SPDX-License-Identifier: Apache-2.0

Generated exclusively from original numeric YUV420 color bars by
SyntheticH264.mm on Apple VideoToolbox. No screen capture, camera, external
image, downloaded video, audio or user input was used. The first 64x64 frame
contains red/green/blue/white vertical bars; the second reverses them. Both
frames are independently forced IDR with SPS/PPS in AnnexB format. The center
pixel (32,32) is blue then green; decoder tests allow compression tolerance.

fixture.json records decoder expectations and exact output/source hashes.
The separate desktop-synthetic-33928f8a13d63e84-provenance.json records native
hardware encoder proof, actual profile bytes and generation inventory. SyntheticH264.mm is the standalone native generator. The private transport used
for the recorded generation is not part of this package or a build prerequisite.
Source and generated fixtures may be redistributed with this package under
its Apache-2.0 license. These are test pixels, not real desktop media.
