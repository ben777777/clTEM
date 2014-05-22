__kernel void clBinnedAtomicPotentialOpt(__global float2* Potential, __global float* clAtomXPos, __global float* clAtomYPos, __global float* clAtomZPos, __global int* clAtomZNum, __constant float* clfParams, __global int* clBlockStartPositions, int width, int height, int slice, int slices, float z, float dz, float pixelscale, int xBlocks, int yBlocks, float MaxX, float MinX, float MaxY, float MinY, int loadBlocksX, int loadBlocksY, int loadSlicesZ, float sigma)
{
	int xid = get_global_id(0);
	int yid = get_global_id(1);
	int Index = xid + width*yid;
	int topz = slice - loadSlicesZ;
	int bottomz = slice + loadSlicesZ;
	float sumz = 0.0f;
	int gx = get_group_id(0);
	int gy = get_group_id(1);
	if(topz < 0 )
		topz = 0;
	if(bottomz >= slices )
		bottomz = slices-1;

	__local float atx[512];
	__local float aty[512];
	__local float atz[512];
	__local int atZ[512];

	for(int k = topz; k <= bottomz; k++)
	{
		for (int j = floor((gy * get_local_size(1) * yBlocks * pixelscale/ (MaxY-MinY)) - loadBlocksY ); j <= ceil(((gy+1) * get_local_size(1) * yBlocks * pixelscale/ (MaxY-MinY)) + loadBlocksY); j++)
		{
			int si = floor((gx * get_local_size(0) * xBlocks * pixelscale / (MaxX-MinX)) - loadBlocksX );
			int ei = ceil(((gx+1) * get_local_size(0) * xBlocks * pixelscale/ (MaxX-MinX)) + loadBlocksX);

			// Check bounds to avoid unneccessarily loading blocks when at edge of sample.
			if(0 <= j && j < yBlocks)
			{
				if(si < 0)
					si = 0;

				if(ei >= xBlocks)
					ei = xBlocks-1;

				//Need list of atoms to load, so we can load in sequence
				int start = clBlockStartPositions[k*xBlocks*yBlocks + xBlocks*j + si];
				int end = clBlockStartPositions[k*xBlocks*yBlocks + xBlocks*j + ei + 1];
				int lid = get_local_id(0) + get_local_size(0)*get_local_id(1);
				int gid = start + get_local_id(0) + get_local_size(0)*get_local_id(1);

				if(lid < end-start)
				{
					atx[lid] = clAtomXPos[gid];
					aty[lid] = clAtomYPos[gid];
					atz[lid] = clAtomZPos[gid];
					atZ[lid] = clAtomZNum[gid];
				}

				barrier(CLK_LOCAL_MEM_FENCE);

				float p2=0;
				for (int l = 0; l < end-start; l++) 
				{
					int ZNum = atZ[l];
					for (int h = 0; h < 10; h++)
					{
						float rad = native_sqrt((xid*pixelscale-atx[l])*(xid*pixelscale-atx[l]) + (yid*pixelscale-aty[l])*(yid*pixelscale-aty[l]) + (z - (h*(dz/10.0f))-atz[l])*(z - (h*(dz/10.0f))-atz[l]));

						if(rad < 0.25f * pixelscale)
							rad = 0.25f * pixelscale;

						float p1 = 0;

						if( rad < 3.0f) // Should also make sure is not too small
						{
							p1 += (150.4121417f * native_recip(rad) * clfParams[(ZNum-1)*12  ]* native_exp( -2.0f*3.141592f*rad*native_sqrt(clfParams[(ZNum-1)*12+1  ])));
							p1 += (150.4121417f * native_recip(rad) * clfParams[(ZNum-1)*12+2]* native_exp( -2.0f*3.141592f*rad*native_sqrt(clfParams[(ZNum-1)*12+2+1])));
							p1 += (150.4121417f * native_recip(rad) * clfParams[(ZNum-1)*12+4]* native_exp( -2.0f*3.141592f*rad*native_sqrt(clfParams[(ZNum-1)*12+4+1])));
							p1 += (266.5157269f * clfParams[(ZNum-1)*12+6] * native_exp (-3.141592f*rad*3.141592f*rad/clfParams[(ZNum-1)*12+6+1]) * native_powr(clfParams[(ZNum-1)*12+6+1],-1.5f));
							p1 += (266.5157269f * clfParams[(ZNum-1)*12+8] * native_exp (-3.141592f*rad*3.141592f*rad/clfParams[(ZNum-1)*12+8+1]) * native_powr(clfParams[(ZNum-1)*12+8+1],-1.5f));
							p1 += (266.5157269f * clfParams[(ZNum-1)*12+10] * native_exp (-3.141592f*rad*3.141592f*rad/clfParams[(ZNum-1)*12+10+1]) * native_powr(clfParams[(ZNum-1)*12+10+1],-1.5f));

							sumz += (h!=0) * (p1+p2)*0.5f;
							p2 = p1;

						}
					}
				}
			}
		}
	}
	if(xid < width && yid < height){
		Potential[Index].x = native_cos((dz/10.0f)*sigma*sumz);
		Potential[Index].y = native_sin((dz/10.0f)*sigma*sumz);
	}
}
