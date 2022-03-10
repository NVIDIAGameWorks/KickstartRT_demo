/*
* Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

cbuffer KickStart_CommonConstants : register(b0)
{
    uint     enableRTReflections;
    uint     enableRTGI;
    uint     enableRTAO;
    uint     enableRTShadows;
    uint     enableDebug;
};

Texture2D<float4> t_Albedo : register(t0);
Texture2D<float4> t_RTReflections : register(t1);
Texture2D<float4> t_RTGI : register(t2);
Texture2D<float4> t_RTAO : register(t3);
Texture2D<float4> t_RTShadows : register(t4);
RWTexture2D<float4> u_OutputTex	: register(u0);

[numthreads(8, 8, 1)]
void main(uint2 globalIndex : SV_DispatchThreadID)
{
    float4 col = u_OutputTex[globalIndex];

    if (enableDebug) {
        col.xyz = t_RTReflections[globalIndex].xyz;
    }
    else {
        if (enableRTReflections) {
            col.xyz += t_RTReflections[globalIndex].xyz;
        }
        if (enableRTGI) {
            col.xyz += t_RTGI[globalIndex].xyz * t_Albedo[globalIndex].xyz;
        }
        if (enableRTAO) {
            col.xyz *= t_RTAO[globalIndex].x;
        }
		// Until we can properly extract shadows to run before main gbuffer, we have the composition in here, but disabled.
		#if 0
        if (enableRTShadows) {
            col.xyz *= (0.1 + t_RTShadows[globalIndex].x);
        }
		#endif
    }

    u_OutputTex[globalIndex] = col;
}