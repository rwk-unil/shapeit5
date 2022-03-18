////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2018 Olivier Delaneau, University of Lausanne
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
////////////////////////////////////////////////////////////////////////////////
#include <models/hmm_scaffold/hmm_scaffold_header.h>

hmm_scaffold::hmm_scaffold(variant_map & _V, genotype_set * _G, conditioning_set & _C, hmm_parameters & _M) : V(_V), G(_G), C(_C), M(_M){
	//
	nstates = vector < unsigned long > (C.n_haplotypes, 0);
	sstates = vector < unsigned long > (C.n_haplotypes, 0);
	for (int i = 0; i < G.n_samples ; i ++) nstates[i] = C.indexes_pbwt_neighbour[i].size();
	for (int i = 1; i < G.n_samples ; i ++) sstates[i] = sstates[i-1] + C.indexes_pbwt_neighbour[i-1].size();
	n_total_states = sstates.back() + nstates.back();

	//
	emit[0] = 1.0f;
	emit[1] = M.ed/M.ee;
	rev_emit[0] = 1.0f;
	rev_emit[1] = M.ee/M.ed;

	//
	K = vector < unsigned int > (n_total_states);
	for (unsigned int h = 0, k = 0 ; h < C.n_haplotypes ; h ++) {
		copy(K.begin() + k, C.indexes_pbwt_neighbour[h/2].begin(), C.indexes_pbwt_neighbour[h/2].end());
		k += C.indexes_pbwt_neighbour[h/2].size();
	}

	//
	bufferA0 = vector < bool > (C.n_haplotypes, false);
	bufferA1 = vector < bool > (C.n_haplotypes, false);
	alpha = vector < float > (n_total_states, 0.0f);
	beta = vector < float > (n_total_states, 1.0f);
	cprobs = vector < vector < prob_state > > (C.n_haplotypes);

	//
	MCMC.allocate(G.n_samples, 50, 20);	//50 iterations w/ 20 burnin
}

hmm_scaffold::~hmm_scaffold() {
	nstates.clear();
	sstates.clear();
	alpha.clear();
	beta.clear();
}

void hmm_scaffold::forward() {
	tac.clock();
	vrb.title("Forward pass");

	//Main loop
	for (int vs = 0 ; vs < C.n_scaffold_variants ; vs ++) {

		//Prefetch alleles at scaffold to speed up compute
		prefetchAlleles0(vs);

		//
		for (int h = 0 ; h < C.n_haplotypes ; h ++) {

			//Update forward with transition from l-1 to l
			if (vs) forward_updateTransitions(vs, h);
			else forward_initTransitions(h);

			//Update forward with emission at l
			forward_updateEmission(vs, h);

			//Normalize forward
			forward_normalize(vs, h);
		}
		vrb.progress("  * Processing", (vs+1)*1.0/C.n_scaffold_variants);
	}
	vrb.bullet("Timing (" + stb.str(tac.rel_time()*1.0/1000, 2) + "s)");
}

void hmm_scaffold::backward(float threshold) {
	tac.clock();

	//Variables
	vector < unsigned int > VC, VR;
	vector < float > alphaXbeta_prev, alphaXbeta_curr;

	vrb.title("Backward pass");

	//Initialization of betas
	for (int h = 0 ; h < C.n_haplotypes ; h ++) backward_initTransitions(h);

	//Main loop
	for (int vs = C.n_scaffold_variants - 2 ; vs >= 0 ; vs --) {

		//Prefetch alleles at scaffold to speed up compute
		prefetchAlleles0(vs);
		prefetchAlleles1(vs+1);

		//Get rare and common unphased variants in the [l, l+1] interval
		V.getCommonVariants(vs, vs+1, VC);
		V.getRareVariants(vs, vs+1, VR);

		//Are there any M/m or ./. at these variants
		G.getUnphasedIndexes(VC, VR, IDX);

		//Loading MCMC conditional probs
		for (int h = 0, idx = 0 ; h < C.n_haplotypes ; h ++) {

			//If there are unphased genotypes in [l, l+1] for sample h/2
			if (IDX.size() > 0 && IDX[idx] == h/2) {
				//Allocate vector to store alphaXbeta products
				alphaXbeta_prev = vector < float > (nstates[h], 0.0f);
				alphaXbeta_curr = vector < float > (nstates[h], 0.0f);

				//Get alphaXbeta product at l+1 [redundant / to be optimized out]
				getAlphaBetaProduct(alphaXbeta_prev);
			}

			//Update backward with emission at l+1
			backward_updateEmission(vs+1, h);

			//Update backward with transition from l+1 to l
			backward_updateTransitions(vs, h);

			//Normalize backward
			backward_normalize(vs, h);

			//Reverse forward from l+1 to l
			forward_reverseEmission(vs, h);
			forward_reverseTransitions(vs, h);
			forward_normalize(vs, h);

			//If there are unphased genotypes in [l, l+1] for sample h/2
			if (IDX.size() > 0 && IDX[idx] == h/2) {
				//Get alphaXbeta product at l
				getAlphaBetaProduct(alphaXbeta_curr);

				//Compress and store alphaXbeta products at l and l + 1
				MCMC.loadStateSpace(h, C.indexes_pbwt_neighbour[h/2], alphaXbeta_curr, alphaXbeta_prev, threshold);
			} else idx ++;
		}

		//Running MCMC at rare variants
		for (int vr = 0 ; vr < VR.size() ; vr ++) {
			MCMC.loadRareUnphasedGenotypes(VR[vr], GS, !V.vec_rare[VR[vr]]->minor);
			MCMC.iterate();
			MCMC.pushRarePhasedGenotypes(VR[vr], GS);
		}

		//Running MCMC at common variants
		for (int vc = 0 ; vc < VC.size() ; vc ++) {
			MCMC.loadCommonUnphasedGenotypes(VC[vc], GS);
			MCMC.iterate();
			MCMC.pushCommonPhasedGenotypes(VC[vc], GS);
		}

		vrb.progress("  * Processing", (C.n_scaffold_variants-vs)*1.0/C.n_scaffold_variants);
	}
	vrb.bullet("Timing (" + stb.str(tac.rel_time()*1.0/1000, 2) + "s)");
}


