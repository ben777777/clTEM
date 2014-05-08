#include "TEMSimulation.h"
#include "clKernelCodes2.h"

TEMSimulation::TEMSimulation(cl_context &context, clQueue* clq, clDevice* cldev, TEMParameters* temparams, STEMParameters* stemparams)
{
	this->context = context;
	this->clq = clq;
	this->cldev = cldev;
	this->TEMParams = temparams;
	this->STEMParams = stemparams;
};

void TEMSimulation::Initialise(int resolution, MultisliceStructure* Structure)
{
	this->resolution = resolution;
	this->AtomicStructure = Structure;

	// Get size of input structure
	float RealSizeX = AtomicStructure->MaximumX-AtomicStructure->MinimumX;
	float RealSizeY = AtomicStructure->MaximumY-AtomicStructure->MinimumY;
	pixelscale = max(RealSizeX,RealSizeY)/resolution;

	// Work out size of each binned block of atoms
	float BlockScaleX = RealSizeX/AtomicStructure->xBlocks; 
	float BlockScaleY = RealSizeY/AtomicStructure->yBlocks;

	// Work out area that is to be simulated
	float SimSizeX = pixelscale * resolution;
	float SimSizeY = SimSizeX;

	float	Pi		= 3.1415926f;	
	float	V		= TEMParams->kilovoltage;
	float	a0		= 52.9177e-012f;
	float	a0a		= a0*1e+010f;
	float	echarge	= 1.6e-019f;
	wavelength		= 6.63e-034f*3e+008f/sqrt((echarge*V*1000*(2*9.11e-031f*9e+016f + echarge*V*1000)))*1e+010f;
	float	sigma	= 2 * Pi * ((511.0f + V) / (2.0f*511.0f + V)) / (V * wavelength);
	float	sigma2	= (2*Pi/(wavelength * V * 1000)) * ((9.11e-031f*9e+016f + echarge*V*1000)/(2*9.11e-031f*9e+016f + echarge*V*1000));
	float	fix		= 300.8242834f/(4*Pi*Pi*a0a*echarge);
	float	V2		= V*1000;

	// Now we can set up frequencies and fourier transforms.

	int imidx = floor(resolution/2 + 0.5);
	int imidy = floor(resolution/2 + 0.5);

	std::vector<float> k0x;
	std::vector<float> k0y;

	float temp;

	for(int i=1 ; i <= resolution ; i++)
	{
		if ((i - 1) > imidx)
			temp = ((i - 1) - resolution)/SimSizeX;
		else temp = (i - 1)/SimSizeX;
		k0x.push_back (temp);
	}

	for(int i=1 ; i <= resolution ; i++)
	{
		if ((i - 1) > imidy)
			temp = ((i - 1) - resolution)/SimSizeY;
		else temp = (i - 1)/SimSizeY;
		k0y.push_back (temp);
	}

	// Find maximum frequency for bandwidth limiting rule....

	 bandwidthkmax=0;

	float	kmaxx = pow((k0x[imidx-1]*1/2),2);
	float	kmaxy = pow((k0y[imidy-1]*1/2),2);
	
	if(kmaxy <= kmaxx)
	{
		bandwidthkmax = kmaxy;
	}
	else 
	{ 
		bandwidthkmax = kmaxx;
	};

	// Bandlimit by FDdz size

	// Upload to device

	clXFrequencies = clCreateBuffer(context, CL_MEM_READ_WRITE, resolution * sizeof( cl_float ), 0, &status);
	clYFrequencies = clCreateBuffer(context, CL_MEM_READ_WRITE, resolution * sizeof( cl_float ), 0, &status);
	clEnqueueWriteBuffer(clq->cmdQueue,clXFrequencies,CL_FALSE,0,resolution*sizeof(cl_float),&k0x[0],0,NULL,NULL);
	clEnqueueWriteBuffer(clq->cmdQueue,clYFrequencies,CL_FALSE,0,resolution*sizeof(cl_float),&k0y[0],0,NULL,NULL);
	
	
	// Setup Fourier Transforms
	FourierTrans = new clFourier(context, clq);
	FourierTrans->Setup(resolution,resolution);


	// Initialise Wavefunctions and Create other buffers...
	clWaveFunction1 = clCreateBuffer(context, CL_MEM_READ_WRITE, resolution * resolution * sizeof( cl_float2 ), 0, &status);
	clWaveFunction2 = clCreateBuffer(context, CL_MEM_READ_WRITE, resolution * resolution * sizeof( cl_float2 ), 0, &status);
	clWaveFunction3 = clCreateBuffer(context, CL_MEM_READ_WRITE, resolution * resolution * sizeof( cl_float2 ), 0, &status);
	
	clPropagator = clCreateBuffer(context, CL_MEM_READ_WRITE, resolution * resolution * sizeof( cl_float2 ), 0, &status);
	clPotential = clCreateBuffer(context, CL_MEM_READ_WRITE, resolution * resolution * sizeof( cl_float2 ), 0, &status);

	// Set initial wavefunction to 1+0i
	clKernel* InitialiseWavefunction = new clKernel(InitialiseWavefunctionSource,context,cldev,"clInitialiseWavefunction",clq);
	InitialiseWavefunction->BuildKernelOld();

	clFinish(clq->cmdQueue);

	float InitialValue = 1.0f;
	InitialiseWavefunction->SetArgT(0,clWaveFunction1);
	InitialiseWavefunction->SetArgT(1,resolution);
	InitialiseWavefunction->SetArgT(2,resolution);
	InitialiseWavefunction->SetArgT(3,InitialValue);

	size_t* WorkSize = new size_t[3];

	WorkSize[0] = resolution;
	WorkSize[1] = resolution;
	WorkSize[2] = 1;

	InitialiseWavefunction->Enqueue(WorkSize);

	BinnedAtomicPotential = new clKernel(BinnedAtomicPotentialSource,context,cldev,"clBinnedAtomicPotential",clq);
	//BinnedAtomicPotential = new clKernel(context,cldev,"clBinnedAtomicPotential",clq);
	//BinnedAtomicPotential->loadProgSource("BinnedAtomicPotential.cl");
	BinnedAtomicPotential->BuildKernelOld();

	// Work out which blocks to load by ensuring we have the entire area around workgroup upto 5 angstroms away...
	int loadblocksx = ceil(sqrtf(9.0f)/((AtomicStructure->MaximumX-AtomicStructure->MinimumX)/(AtomicStructure->xBlocks)));
	int loadblocksy = ceil(sqrtf(9.0f)/((AtomicStructure->MaximumY-AtomicStructure->MinimumY)/(AtomicStructure->yBlocks)));
	int loadblocksz = ceil(sqrtf(9.0f)/AtomicStructure->dz);

	// Set some of the arguments which dont change each iteration
	BinnedAtomicPotential->SetArgT(0,clPotential);
	BinnedAtomicPotential->SetArgT(1,AtomicStructure->clAtomx);
	BinnedAtomicPotential->SetArgT(2,AtomicStructure->clAtomy);
	BinnedAtomicPotential->SetArgT(3,AtomicStructure->clAtomz);
	BinnedAtomicPotential->SetArgT(4,AtomicStructure->clAtomZ);
	BinnedAtomicPotential->SetArgT(5,AtomicStructure->AtomicStructureParameterisation);
	BinnedAtomicPotential->SetArgT(6,AtomicStructure->clBlockStartPositions);
	BinnedAtomicPotential->SetArgT(7,resolution);
	BinnedAtomicPotential->SetArgT(8,resolution);
	BinnedAtomicPotential->SetArgT(12,AtomicStructure->dz);
	BinnedAtomicPotential->SetArgT(13,pixelscale);
	BinnedAtomicPotential->SetArgT(14,AtomicStructure->xBlocks);
	BinnedAtomicPotential->SetArgT(15,AtomicStructure->yBlocks);
	BinnedAtomicPotential->SetArgT(16,AtomicStructure->MaximumX);
	BinnedAtomicPotential->SetArgT(17,AtomicStructure->MinimumX);
	BinnedAtomicPotential->SetArgT(18,AtomicStructure->MaximumY);
	BinnedAtomicPotential->SetArgT(19,AtomicStructure->MinimumY);
	BinnedAtomicPotential->SetArgT(20,loadblocksx);
	BinnedAtomicPotential->SetArgT(21,loadblocksy);
	BinnedAtomicPotential->SetArgT(22,loadblocksz);
	BinnedAtomicPotential->SetArgT(23,sigma2); // Not sure why i am using sigma 2 and not sigma...
	
	// Also need to generate propagator.
	GeneratePropagator = new clKernel(context,cldev,"clGeneratePropagator",clq);
	GeneratePropagator->loadProgSource("GeneratePropagator.cl");
	GeneratePropagator->BuildKernel();

	GeneratePropagator->SetArgT(0,clPropagator);
	GeneratePropagator->SetArgT(1,clXFrequencies);
	GeneratePropagator->SetArgT(2,clYFrequencies);
	GeneratePropagator->SetArgT(3,resolution);
	GeneratePropagator->SetArgT(4,resolution);
	GeneratePropagator->SetArgT(5,AtomicStructure->dz); // Is this the right dz? (Propagator needs slice thickness not spacing between atom bins)
	GeneratePropagator->SetArgT(6,wavelength);
	GeneratePropagator->SetArgT(7,bandwidthkmax);

	GeneratePropagator->Enqueue(WorkSize);
	
	// And multiplication kernel
	ComplexMultiply = new clKernel(context,cldev,"clComplexMultiply",clq);
	ComplexMultiply->loadProgSource("Multiply.cl");
	ComplexMultiply->BuildKernel();

	ComplexMultiply->SetArgT(3,resolution);
	ComplexMultiply->SetArgT(4,resolution);

	clFinish(clq->cmdQueue);
};

void TEMSimulation::InitialiseSTEM(int resolution, MultisliceStructure* Structure)
{
	this->resolution = resolution;
	this->AtomicStructure = Structure;

	// Get size of input structure
	float RealSizeX = AtomicStructure->MaximumX-AtomicStructure->MinimumX;
	float RealSizeY = AtomicStructure->MaximumY-AtomicStructure->MinimumY;
	pixelscale = max(RealSizeX,RealSizeY)/resolution;

	// Work out size of each binned block of atoms
	float BlockScaleX = RealSizeX/AtomicStructure->xBlocks; 
	float BlockScaleY = RealSizeY/AtomicStructure->yBlocks;

	// Work out area that is to be simulated
	float SimSizeX = pixelscale * resolution;
	float SimSizeY = SimSizeX;

	float	Pi		= 3.1415926f;	
	float	V		= TEMParams->kilovoltage;
	float	a0		= 52.9177e-012f;
	float	a0a		= a0*1e+010f;
	float	echarge	= 1.6e-019f;
	wavelength		= 6.63e-034f*3e+008f/sqrt((echarge*V*1000*(2*9.11e-031f*9e+016f + echarge*V*1000)))*1e+010f;
	float	sigma	= 2 * Pi * ((511.0f + V) / (2.0f*511.0f + V)) / (V * wavelength);
	float	sigma2	= (2*Pi/(wavelength * V * 1000)) * ((9.11e-031f*9e+016f + echarge*V*1000)/(2*9.11e-031f*9e+016f + echarge*V*1000));
	float	fix		= 300.8242834f/(4*Pi*Pi*a0a*echarge);
	float	V2		= V*1000;

	// Now we can set up frequencies and fourier transforms.

	int imidx = floor(resolution/2 + 0.5);
	int imidy = floor(resolution/2 + 0.5);

	std::vector<float> k0x;
	std::vector<float> k0y;

	float temp;

	for(int i=1 ; i <= resolution ; i++)
	{
		if ((i - 1) > imidx)
			temp = ((i - 1) - resolution)/SimSizeX;
		else temp = (i - 1)/SimSizeX;
		k0x.push_back (temp);
	}

	for(int i=1 ; i <= resolution ; i++)
	{
		if ((i - 1) > imidy)
			temp = ((i - 1) - resolution)/SimSizeY;
		else temp = (i - 1)/SimSizeY;
		k0y.push_back (temp);
	}

	// Find maximum frequency for bandwidth limiting rule....

	 bandwidthkmax=0;

	float	kmaxx = pow((k0x[imidx-1]*1/2),2);
	float	kmaxy = pow((k0y[imidy-1]*1/2),2);
	
	if(kmaxy <= kmaxx)
	{
		bandwidthkmax = kmaxy;
	}
	else 
	{ 
		bandwidthkmax = kmaxx;
	};

	// Bandlimit by FDdz size

	// Upload to device

	clXFrequencies = clCreateBuffer(context, CL_MEM_READ_WRITE, resolution * sizeof( cl_float ), 0, &status);
	clYFrequencies = clCreateBuffer(context, CL_MEM_READ_WRITE, resolution * sizeof( cl_float ), 0, &status);
	clEnqueueWriteBuffer(clq->cmdQueue,clXFrequencies,CL_FALSE,0,resolution*sizeof(cl_float),&k0x[0],0,NULL,NULL);
	clEnqueueWriteBuffer(clq->cmdQueue,clYFrequencies,CL_FALSE,0,resolution*sizeof(cl_float),&k0y[0],0,NULL,NULL);
	
	// Setup Fourier Transforms
	FourierTrans = new clFourier(context, clq);
	FourierTrans->Setup(resolution,resolution);

	// Initialise Wavefunctions and Create other buffers...
	clWaveFunction1 = clCreateBuffer(context, CL_MEM_READ_WRITE, resolution * resolution * sizeof( cl_float2 ), 0, &status);
	clWaveFunction2 = clCreateBuffer(context, CL_MEM_READ_WRITE, resolution * resolution * sizeof( cl_float2 ), 0, &status);
	clWaveFunction3 = clCreateBuffer(context, CL_MEM_READ_WRITE, resolution * resolution * sizeof( cl_float2 ), 0, &status);
	
	clPropagator = clCreateBuffer(context, CL_MEM_READ_WRITE, resolution * resolution * sizeof( cl_float2 ), 0, &status);
	clPotential = clCreateBuffer(context, CL_MEM_READ_WRITE, resolution * resolution * sizeof( cl_float2 ), 0, &status);

	// Set initial wavefunction to 1+0i
	clKernel* InitialiseWavefunction = new clKernel(InitialiseWavefunctionSource,context,cldev,"clInitialiseWavefunction",clq);
	InitialiseWavefunction->BuildKernelOld();

	float InitialValue = 1.0f;
	InitialiseWavefunction->SetArgT(0,clWaveFunction1);
	InitialiseWavefunction->SetArgT(1,resolution);
	InitialiseWavefunction->SetArgT(2,resolution);
	InitialiseWavefunction->SetArgT(3,InitialValue);

	size_t* WorkSize = new size_t[3];

	WorkSize[0] = resolution;
	WorkSize[1] = resolution;
	WorkSize[2] = 1;

	InitialiseWavefunction->Enqueue(WorkSize);


	//BinnedAtomicPotential = new clKernel(BinnedAtomicPotentialSource,context,cldev,"clBinnedAtomicPotential",clq);
	BinnedAtomicPotential = new clKernel(context,cldev,"clBinnedAtomicPotential",clq);
	BinnedAtomicPotential->loadProgSource("BinnedAtomicPotential.cl");
	BinnedAtomicPotential->BuildKernel();

	// Work out which blocks to load by ensuring we have the entire area around workgroup upto 5 angstroms away...
	int loadblocksx = ceil(sqrtf(9.0f)/((AtomicStructure->MaximumX-AtomicStructure->MinimumX)/(AtomicStructure->xBlocks)));
	int loadblocksy = ceil(sqrtf(9.0f)/((AtomicStructure->MaximumY-AtomicStructure->MinimumY)/(AtomicStructure->yBlocks)));
	int loadblocksz = ceil(sqrtf(9.0f)/AtomicStructure->dz);

	// Set some of the arguments which dont change each iteration
	BinnedAtomicPotential->SetArgT(0,clPotential);
	BinnedAtomicPotential->SetArgT(1,AtomicStructure->clAtomx);
	BinnedAtomicPotential->SetArgT(2,AtomicStructure->clAtomy);
	BinnedAtomicPotential->SetArgT(3,AtomicStructure->clAtomz);
	BinnedAtomicPotential->SetArgT(4,AtomicStructure->clAtomZ);
	BinnedAtomicPotential->SetArgT(5,AtomicStructure->AtomicStructureParameterisation);
	BinnedAtomicPotential->SetArgT(6,AtomicStructure->clBlockStartPositions);
	BinnedAtomicPotential->SetArgT(7,resolution);
	BinnedAtomicPotential->SetArgT(8,resolution);
	BinnedAtomicPotential->SetArgT(12,AtomicStructure->dz);
	BinnedAtomicPotential->SetArgT(13,pixelscale);
	BinnedAtomicPotential->SetArgT(14,AtomicStructure->xBlocks);
	BinnedAtomicPotential->SetArgT(15,AtomicStructure->yBlocks);
	BinnedAtomicPotential->SetArgT(16,AtomicStructure->MaximumX);
	BinnedAtomicPotential->SetArgT(17,AtomicStructure->MinimumX);
	BinnedAtomicPotential->SetArgT(18,AtomicStructure->MaximumY);
	BinnedAtomicPotential->SetArgT(19,AtomicStructure->MinimumY);
	BinnedAtomicPotential->SetArgT(20,loadblocksx);
	BinnedAtomicPotential->SetArgT(21,loadblocksy);
	BinnedAtomicPotential->SetArgT(22,loadblocksz);
	BinnedAtomicPotential->SetArgT(23,sigma2); // Not sure why i am using sigma 2 and not sigma...

	
	// Also need to generate propagator.
	GeneratePropagator = new clKernel(context,cldev,"clGeneratePropagator",clq);
	GeneratePropagator->loadProgSource("GeneratePropagator.cl");
	GeneratePropagator->BuildKernel();

	GeneratePropagator->SetArgT(0,clPropagator);
	GeneratePropagator->SetArgT(1,clXFrequencies);
	GeneratePropagator->SetArgT(2,clYFrequencies);
	GeneratePropagator->SetArgT(3,resolution);
	GeneratePropagator->SetArgT(4,resolution);
	GeneratePropagator->SetArgT(5,AtomicStructure->dz); // Is this the right dz?
	GeneratePropagator->SetArgT(6,wavelength);
	GeneratePropagator->SetArgT(7,bandwidthkmax);

	GeneratePropagator->Enqueue(WorkSize);
	
	// And multiplication kernel
	ComplexMultiply = new clKernel(context,cldev,"clComplexMultiply",clq);
	ComplexMultiply->loadProgSource("Multiply.cl");
	ComplexMultiply->BuildKernel();

	ComplexMultiply->SetArgT(3,resolution);
	ComplexMultiply->SetArgT(4,resolution);

	clFinish(clq->cmdQueue);
};

void TEMSimulation::MultisliceStep(int stepno, int steps)
{
	/*	
		float dz = 1;

		//TODO: If ever change change everywhere...
		dim3 dimBlock(32,8,1);
		dim3 dimGrid((resolutionX + dimBlock.x-1)/dimBlock.x,(resolutionY + dimBlock.y-1)/dimBlock.y,1);
		dim3 dimGridbig((2*resolutionX + dimBlock.x-1)/dimBlock.x,(2*resolutionY + dimBlock.y-1)/dimBlock.y,1);

	
		float currentz = MaximumZ - MinimumZ - dz*iteration;
			
		// NOTE getting some NaN's in Psi.

		binnedAtomPKernel<<<dimGrid,dimBlock>>>(devV,PixelScale,atomMemories.fparamsdev,atomMemories.devAtomZPos,atomMemories.devAtomXPos,atomMemories.devAtomYPos,atomMemories.devAtomZNum,atomMemories.devBlockStartPositions,dz,currentz,ceil((MaximumZ-MinimumZ)/dz),sigma2);
		
		if(iteration==1)
		{
			multiplicationKernel<<<dimGrid,dimBlock>>>(devV,PsiMinus,PsiPlus,1); // Think initialized psi with one not psiminus
		}
		else
		{
			multiplicationKernel<<<dimGrid,dimBlock>>>(devV,Psi,PsiPlus,1);
		}

		//  cudaMemcpy(checkval,&devV[40000],1*sizeof(cuComplex),cudaMemcpyDeviceToHost);
		//	cout << checkval[0].x <<" , " << checkval[0].y << endl;
		
		if(iteration==1)
			CreatePropsKernel<<<dimGrid,dimBlock>>>(xFrequencies,yFrequencies,dz,wavel,PsiMinus,kmax);
		
		cufftExecC2C(plan,PsiPlus,Psi,CUFFT_FORWARD);
		multiplicationKernel<<<dimGrid,dimBlock>>>(Psi,PsiMinus,PsiPlus,normalisingfactor);
		cufftExecC2C(plan,PsiPlus,Psi,CUFFT_INVERSE);
		normalisingKernel<<<dimGrid,dimBlock>>>(Psi,normalisingfactor);
		
		*/


	// Work out current z position based on step size and current step
	// Should be one set of bins for each individual slice
	

	int slice = stepno - 1;
	int slices = steps;

	float currentz = AtomicStructure->MaximumZ - slice * AtomicStructure->dz;

	BinnedAtomicPotential->SetArgT(9,slice);
	BinnedAtomicPotential->SetArgT(10,slices);
	BinnedAtomicPotential->SetArgT(11,currentz);

	size_t* Work = new size_t[3];

	Work[0]=resolution;
	Work[1]=resolution;
	Work[2]=1;

	size_t* LocalWork = new size_t[3];

	LocalWork[0]=32;
	LocalWork[1]=8;
	LocalWork[2]=1;

	BinnedAtomicPotential->Enqueue3D(Work,LocalWork);

	// Now for the rest of the multislice steps

	//Multiply with wavefunction
	ComplexMultiply->SetArgT(0,clPotential);
	ComplexMultiply->SetArgT(1,clWaveFunction1);
	ComplexMultiply->SetArgT(2,clWaveFunction2);
	ComplexMultiply->Enqueue(Work);

	// Propagate
	FourierTrans->Enqueue(clWaveFunction2,clWaveFunction3,CLFFT_FORWARD);

	ComplexMultiply->SetArgT(0,clWaveFunction3);
	ComplexMultiply->SetArgT(1,clPropagator);
	ComplexMultiply->SetArgT(2,clWaveFunction2);
	ComplexMultiply->Enqueue(Work);

		
	FourierTrans->Enqueue(clWaveFunction2,clWaveFunction1,CLFFT_BACKWARD);

	clFinish(clq->cmdQueue);
	
};

void TEMSimulation::GetCTEMImage(float* data, int resolution)
{
	// Original data is complex so copy complex version down first
	std::vector<cl_float2> compdata;
	compdata.resize(resolution*resolution);

	clEnqueueReadBuffer(clq->cmdQueue,clWaveFunction1,CL_TRUE,0,resolution*resolution*sizeof(cl_float2),&compdata[0],0,NULL,NULL);

	float max = CL_FLT_MIN;
	float min = CL_MAXFLOAT;

	for(int i = 0; i < resolution * resolution; i++)
	{
		// Get absolute value for display...	
		data[i] = sqrt(compdata[i].s[0]*compdata[i].s[0] + compdata[i].s[1]*compdata[i].s[1]);
	
		// Find max,min for contrast limits
		if(data[i] > max)
			max = data[i];
		if(data[i] < min)
			min = data[i];	
	}

	imagemin = min;
	imagemax = max;
};
