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
    uint     _pad0;

    uint     enableDebug;
    uint     enableYCoCgToLienarOnRTReflections;
    uint     enableYCoCgToLienarOnRTGI;
    uint     _pad1;
};

Texture2D<float4> t_Albedo : register(t0);
Texture2D<float4> t_RTReflections : register(t1);
Texture2D<float4> t_RTGI : register(t2);
Texture2D<float4> t_RTAO : register(t3);
Texture2D<float4> t_RTShadows : register(t4);
RWTexture2D<float4> u_OutputTex	: register(u0);

// Since NRD's REBLUR outputs the result with YCoCg color space and KickStartRT doesn't want to add an extra render pass to simply decode it to RGB space, applications need to decode it.
// NRD has prvided a utility function, REBLUR_BackEnd_UnpackRadianceAndNormHitDist() to decode, but for simplicity,
// this sample application decodes it with a local function instead of including NRD shader codes.
float3 YCoCgToLinear(const float3 color )
{
    float t = color.x - color.z;

    float3 r;
    r.y = color.x + color.z;
    r.x = t + color.y;
    r.z = t - color.y;

    return max( r, 0.0 );
}

[numthreads(8, 8, 1)]
void main(uint2 globalIndex : SV_DispatchThreadID)
{
    float4 col = u_OutputTex[globalIndex];

    if (enableDebug) {
        float3 t = t_RTReflections[globalIndex].xyz;

        if (enableYCoCgToLienarOnRTReflections) {
            t = YCoCgToLinear(t);
        }

        col.xyz = t;
    }
    else {
        if (enableRTReflections) {
            float3 t = t_RTReflections[globalIndex].xyz;

            if (enableYCoCgToLienarOnRTReflections) {
                t = YCoCgToLinear(t);
            }

            col.xyz += t;
        }
        if (enableRTGI) {
            float3 t = t_RTGI[globalIndex].xyz;

            if (enableYCoCgToLienarOnRTGI) {
                t = YCoCgToLinear(t);
            }

            col.xyz += t * t_Albedo[globalIndex].xyz;
        }
        if (enableRTAO) {
            col.xyz *= t_RTAO[globalIndex].x;
        }
    }

    u_OutputTex[globalIndex] = col;
}
