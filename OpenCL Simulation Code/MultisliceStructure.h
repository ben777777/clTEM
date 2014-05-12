#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include "CL\cl.h"
#include "clKernel.h"

#pragma once

struct Atom
{
	int Z;
	float x;
	float y;
	float z;
};

using namespace std;

class MultisliceStructure
{

public:
	cl_context context;
	clQueue* clq;
	clDevice* cldev;
	cl_int status;

	cl_mem clAtomx;
	cl_mem clAtomy;
	cl_mem clAtomz;
	cl_mem clAtomZ;
	cl_mem clBlockStartPositions;

	// OpenCL Memory
	cl_mem AtomicStructureParameterisation;

	MultisliceStructure(cl_context &context, clQueue* clq, clDevice* cldev);

	// Import atoms from xyz filepath
	void ImportAtoms(std::string filepath);
	int SortAtoms();
	void ClearStructure();
	// Convert atomic symbol i.e. Fe to Atomic Number e.g. 53
	static int GetZNum(std::string AtomSymbol);


	std::vector<Atom> Atoms;

	// Coordinates encompassing all atom positions
	float MaximumX;
	float MinimumX;
	float MaximumY;
	float MinimumY;
	float MaximumZ;
	float MinimumZ;

	int Length;

	int xBlocks;
	int yBlocks;
	float dz;
	int nSlices;

};