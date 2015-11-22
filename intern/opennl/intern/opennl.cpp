/** \file opennl/intern/opennl.c
 *  \ingroup opennlintern
 */
/*
 *
 *  OpenNL: Numerical Library
 *  Copyright (C) 2004 Bruno Levy
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *  If you modify this software, you should include a notice giving the
 *  name of the person performing the modification, the date of modification,
 *  and the reason for such modification.
 *
 *  Contact: Bruno Levy
 *
 *	 levy@loria.fr
 *
 *	 ISA Project
 *	 LORIA, INRIA Lorraine, 
 *	 Campus Scientifique, BP 239
 *	 54506 VANDOEUVRE LES NANCY CEDEX 
 *	 FRANCE
 *
 *  Note that the GNU General Public License does not permit incorporating
 *  the Software into proprietary programs. 
 */

#include "ONL_opennl.h"

#include <Eigen/Sparse>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <vector>

/* Eigen data structures */

typedef Eigen::SparseMatrix<double, Eigen::ColMajor> EigenSparseMatrix;
typedef Eigen::SparseLU<EigenSparseMatrix> EigenSparseSolver;
typedef Eigen::VectorXd EigenVectorX;
typedef Eigen::Triplet<double> EigenTriplet;

/* NLContext data structure */

typedef struct {
	NLuint index;
	NLdouble value;
} NLCoeff;

typedef struct {
	NLdouble value[4];
	NLboolean locked;
	NLuint index;
	std::vector<NLCoeff> a;
} NLVariable;

#define NL_STATE_INITIAL            0
#define NL_STATE_SYSTEM             1
#define NL_STATE_MATRIX             2
#define NL_STATE_MATRIX_CONSTRUCTED 3
#define NL_STATE_SYSTEM_CONSTRUCTED 4
#define NL_STATE_SYSTEM_SOLVED      5

struct NLContext {
	NLenum state;

	NLuint n;
	NLuint m;

	std::vector<EigenTriplet> Mtriplets;
	EigenSparseMatrix M;
	EigenSparseMatrix MtM;
	std::vector<EigenVectorX> b;
	std::vector<EigenVectorX> Mtb;
	std::vector<EigenVectorX> x;

	EigenSparseSolver *sparse_solver;

	NLuint nb_variables;
	std::vector<NLVariable> variable;

	NLuint nb_rows;
	NLuint nb_rhs;

	NLboolean least_squares;
	NLboolean solve_again;
};

static NLContext* __nlCurrentContext = NULL;

NLContext *nlNewContext(void)
{
	NLContext* result = new NLContext();
	result->state = NL_STATE_INITIAL;
	result->nb_rhs = 1;
	nlMakeCurrent(result);
	return result;
}

void nlDeleteContext(NLContext *context)
{
	if(__nlCurrentContext == context)
		__nlCurrentContext = NULL;

	context->M.resize(0, 0);
	context->MtM.resize(0, 0);
	context->b.clear();
	context->Mtb.clear();
	context->x.clear();
	context->variable.clear();

	delete context->sparse_solver;
	context->sparse_solver = NULL;

	delete context;
}

void nlMakeCurrent(NLContext *context)
{
	__nlCurrentContext = context;
}

NLContext *nlGetCurrent(void)
{
	return __nlCurrentContext;
}

static void __nlCheckState(NLenum state)
{
	assert(__nlCurrentContext->state == state);
}

static void __nlTransition(NLenum from_state, NLenum to_state)
{
	__nlCheckState(from_state);
	__nlCurrentContext->state = to_state;
}

/* Get/Set parameters */

void nlSolverParameteri(NLenum pname, NLint param)
{
	__nlCheckState(NL_STATE_INITIAL);
	switch(pname) {
	case NL_NB_VARIABLES: {
		assert(param > 0);
		__nlCurrentContext->nb_variables = (NLuint)param;
	} break;
	case NL_NB_ROWS: {
		assert(param > 0);
		__nlCurrentContext->nb_rows = (NLuint)param;
	} break;
	case NL_LEAST_SQUARES: {
		__nlCurrentContext->least_squares = (NLboolean)param;
	} break;
	case NL_NB_RIGHT_HAND_SIDES: {
		__nlCurrentContext->nb_rhs = (NLuint)param;
	} break;
	default: {
		assert(0);
	} break;
	}
}

/* Get/Set Lock/Unlock variables */

void nlSetVariable(NLuint rhsindex, NLuint index, NLdouble value)
{
	__nlCheckState(NL_STATE_SYSTEM);
	__nlCurrentContext->variable[index].value[rhsindex] = value;	
}

NLdouble nlGetVariable(NLuint rhsindex, NLuint index)
{
	assert(__nlCurrentContext->state != NL_STATE_INITIAL);
	return __nlCurrentContext->variable[index].value[rhsindex];
}

void nlLockVariable(NLuint index)
{
	__nlCheckState(NL_STATE_SYSTEM);
	__nlCurrentContext->variable[index].locked = true;
}

void nlUnlockVariable(NLuint index)
{
	__nlCheckState(NL_STATE_SYSTEM);
	__nlCurrentContext->variable[index].locked = false;
}

/* System construction */

static void __nlVariablesToVector()
{
	NLContext *context = __nlCurrentContext;
	NLuint i, j, nb_rhs;

	nb_rhs= context->nb_rhs;

	for(i=0; i<context->nb_variables; i++) {
		NLVariable* v = &(context->variable[i]);
		if(!v->locked) {
			for(j=0; j<nb_rhs; j++)
				context->x[j][v->index] = v->value[j];
		}
	}
}

static void __nlVectorToVariables()
{
	NLContext *context = __nlCurrentContext;
	NLuint i, j, nb_rhs;

	nb_rhs= context->nb_rhs;

	for(i=0; i<context->nb_variables; i++) {
		NLVariable* v = &(context->variable[i]);
		if(!v->locked) {
			for(j=0; j<nb_rhs; j++)
				v->value[j] = context->x[j][v->index];
		}
	}
}

static void __nlBeginSystem()
{
	assert(__nlCurrentContext->nb_variables > 0);

	if (__nlCurrentContext->solve_again)
		__nlTransition(NL_STATE_SYSTEM_SOLVED, NL_STATE_SYSTEM);
	else {
		__nlTransition(NL_STATE_INITIAL, NL_STATE_SYSTEM);

		__nlCurrentContext->variable.resize(__nlCurrentContext->nb_variables);
	}
}

static void __nlEndSystem()
{
	__nlTransition(NL_STATE_MATRIX_CONSTRUCTED, NL_STATE_SYSTEM_CONSTRUCTED);	
}

static void __nlBeginMatrix()
{
	NLuint i;
	NLuint m = 0, n = 0;
	NLContext *context = __nlCurrentContext;

	__nlTransition(NL_STATE_SYSTEM, NL_STATE_MATRIX);

	if (!context->solve_again) {
		for(i=0; i<context->nb_variables; i++) {
			if(context->variable[i].locked)
				context->variable[i].index = ~0;
			else
				context->variable[i].index = n++;
		}

		m = (context->nb_rows == 0)? n: context->nb_rows;

		context->m = m;
		context->n = n;

		context->b.resize(context->nb_rhs);
		context->x.resize(context->nb_rhs);

		for (i=0; i<context->nb_rhs; i++) {
			context->b[i].setZero(m);
			context->x[i].setZero(n);
		}
	}
	else {
		/* need to recompute b only, A is not constructed anymore */
		for (i=0; i<context->nb_rhs; i++)
			context->b[i].setZero(context->m);
	}

	__nlVariablesToVector();
}

static void __nlEndMatrixRHS(NLuint rhs)
{
	NLContext *context = __nlCurrentContext;
	NLVariable *variable;
	NLuint i, j;

	EigenVectorX& b = context->b[rhs];

	for(i=0; i<__nlCurrentContext->nb_variables; i++) {
		variable = &(context->variable[i]);

		if(variable->locked) {
			std::vector<NLCoeff>& a = variable->a;

			for(j=0; j<a.size(); j++) {
				b[a[j].index] -= a[j].value*variable->value[rhs];
			}
		}
	}

	if(context->least_squares)
		context->Mtb[rhs] = context->M.transpose() * b;
}

static void __nlEndMatrix()
{
	NLContext *context = __nlCurrentContext;

	__nlTransition(NL_STATE_MATRIX, NL_STATE_MATRIX_CONSTRUCTED);	
	
	if(!context->solve_again) {
		context->M.resize(context->m, context->n);
		context->M.setFromTriplets(context->Mtriplets.begin(), context->Mtriplets.end());
		context->Mtriplets.clear();

		if(context->least_squares) {
			context->MtM = context->M.transpose() * context->M;

			context->Mtb.resize(context->nb_rhs);
			for (NLuint rhs=0; rhs<context->nb_rhs; rhs++)
				context->Mtb[rhs].setZero(context->n);
		}
	}

	for (NLuint rhs=0; rhs<context->nb_rhs; rhs++)
		__nlEndMatrixRHS(rhs);
}

void nlMatrixAdd(NLuint row, NLuint col, NLdouble value)
{
	NLContext *context = __nlCurrentContext;

	__nlCheckState(NL_STATE_MATRIX);

	if(context->solve_again)
		return;

	if (!context->least_squares && context->variable[row].locked);
	else if (context->variable[col].locked) {
		if(!context->least_squares)
			row = context->variable[row].index;

		NLCoeff coeff = {row, value};
		context->variable[col].a.push_back(coeff);
	}
	else {
		if(!context->least_squares)
			row = context->variable[row].index;
		col = context->variable[col].index;

		// direct insert into matrix is too slow, so use triplets
		EigenTriplet triplet(row, col, value);
		context->Mtriplets.push_back(triplet);
	}
}

void nlRightHandSideAdd(NLuint rhsindex, NLuint index, NLdouble value)
{
	NLContext *context = __nlCurrentContext;

	__nlCheckState(NL_STATE_MATRIX);

	if(context->least_squares) {
		context->b[rhsindex][index] += value;
	}
	else {
		if(!context->variable[index].locked) {
			index = context->variable[index].index;
			context->b[rhsindex][index] += value;
		}
	}
}

void nlRightHandSideSet(NLuint rhsindex, NLuint index, NLdouble value)
{
	NLContext *context = __nlCurrentContext;

	__nlCheckState(NL_STATE_MATRIX);

	if(context->least_squares) {
		context->b[rhsindex][index] = value;
	}
	else {
		if(!context->variable[index].locked) {
			index = context->variable[index].index;
			context->b[rhsindex][index] = value;
		}
	}
}

void nlBegin(NLenum prim)
{
	switch(prim) {
	case NL_SYSTEM: {
		__nlBeginSystem();
	} break;
	case NL_MATRIX: {
		__nlBeginMatrix();
	} break;
	default: {
		assert(0);
	}
	}
}

void nlEnd(NLenum prim)
{
	switch(prim) {
	case NL_SYSTEM: {
		__nlEndSystem();
	} break;
	case NL_MATRIX: {
		__nlEndMatrix();
	} break;
	default: {
		assert(0);
	}
	}
}

void nlPrintMatrix(void)
{
	NLContext *context = __nlCurrentContext;

	std::cout << "A:" << context->M << std::endl;

	for(NLuint rhs=0; rhs<context->nb_rhs; rhs++)
		std::cout << "b " << rhs << ":" << context->b[rhs] << std::endl;

	if (context->MtM.rows() && context->MtM.cols())
		std::cout << "AtA:" << context->MtM << std::endl;
}

/* Solving */

NLboolean nlSolve(NLboolean solveAgain)
{
	NLContext *context = __nlCurrentContext;
	NLboolean result = true;

	__nlCheckState(NL_STATE_SYSTEM_CONSTRUCTED);

	if (!__nlCurrentContext->solve_again) {
		EigenSparseMatrix& M = (context->least_squares)? context->MtM: context->M;

		assert(M.rows() == M.cols());

		/* Convert M to compressed column format */
		M.makeCompressed();

		/* Perform sparse LU factorization */
		EigenSparseSolver *sparse_solver = new EigenSparseSolver();
		context->sparse_solver = sparse_solver;

		sparse_solver->analyzePattern(M);
		sparse_solver->factorize(M);

		result = (sparse_solver->info() == Eigen::Success);

		/* Free M, don't need it anymore at this point */
		M.resize(0, 0);
	}

	if (result) {
		/* Solve each right hand side */
		for(NLuint rhs=0; rhs<context->nb_rhs; rhs++) {
			EigenVectorX& b = (context->least_squares)? context->Mtb[rhs]: context->b[rhs];
			context->x[rhs] = context->sparse_solver->solve(b);

			if (context->sparse_solver->info() != Eigen::Success)
				result = false;
		}

		if (result) {
			__nlVectorToVariables();

			if (solveAgain)
				__nlCurrentContext->solve_again = true;

			__nlTransition(NL_STATE_SYSTEM_CONSTRUCTED, NL_STATE_SYSTEM_SOLVED);
		}
	}

	return result;
}

