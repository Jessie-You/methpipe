/*    Copyright (C) 2014 University of Southern California and
 *                       Andrew D. Smith and Fang Fang and Benjamin Decato
 *
 *    Authors: Fang Fang and Benjamin Decato and Andrew D. Smith
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "EpireadStats.hpp"

#include <vector>
#include <string>
#include <cmath>
#include <cassert>
#include <numeric>
#include <limits>
#include <iostream>

#include <gsl/gsl_sf.h>
#include <gsl/gsl_cdf.h>

using std::string;
using std::vector;
using std::cerr;
using std::isfinite;


static const double EPIREAD_STATS_TOLERANCE = 1e-20;

inline bool
is_meth(const epiread &r, const size_t pos) {return (r.seq[pos] == 'C');}


inline bool
un_meth(const epiread &r, const size_t pos) {return (r.seq[pos] == 'T');}

double
log_likelihood(const epiread &r, const vector<double> &a) {
  double ll = 0.0;
  for (size_t i = 0; i < r.seq.length(); ++i) {
    if (is_meth(r, i) || un_meth(r, i)) {
      const double val = (is_meth(r, i) ? a[r.pos + i] : (1.0 - a[r.pos + i]));
      assert(isfinite(log(val)));
      ll += log(val);
    }
  }
  return ll;
}

double
log_likelihood(const epiread &r, const double z,
 	       const vector<double> &a1, const vector<double> &a2) {
  return z*log_likelihood(r, a1) + (1.0 - z)*log_likelihood(r, a2);
}

double
log_likelihood(const epiread &r, const vector<double> &a1,
	       const vector<double> &a2) {
  return log(exp(log_likelihood(r, a1)) + exp(log_likelihood(r, a2)));
}

double
log_likelihood(const vector<epiread> &reads, const vector<double> &indicators,
	       const vector<double> &a1, const vector<double> &a2) {
  double ll = 0.0;
  for (size_t i = 0; i < reads.size(); ++i)
    ll += log_likelihood(reads[i], indicators[i], a1, a2);
  return ll;
}


static double
expectation_step(const vector<epiread> &reads, const double mixing,
		 const vector<double> &a1, const vector<double> &a2, 
		 vector<double> &indicators) {
  const double log_mixing1 = log(mixing);
  const double log_mixing2 = log(1.0 - mixing);
  assert(isfinite(log_mixing1) && isfinite(log_mixing2));
  
  double score = 0;
  for (size_t i = 0; i < reads.size(); ++i) {
    const double ll1 = log_mixing1 + log_likelihood(reads[i], a1);
    const double ll2 = log_mixing2 + log_likelihood(reads[i], a2);
    assert(isfinite(ll1) && isfinite(ll2));
    const double log_denom = log(exp(ll1) + exp(ll2));
    score += log_denom;
    indicators[i] = exp(ll1 - log_denom);
    assert(isfinite(log_denom) && isfinite(indicators[i]));
  }
  return score;
}


void
fit_epiallele(const vector<epiread> &reads, 
	      const vector<double> &indicators, vector<double> &a) {
  const size_t n_cpgs = a.size();
  vector<double> meth(n_cpgs, 0.0), total(n_cpgs, 0.0);
  for (size_t i = 0; i < reads.size(); ++i) {
    const size_t start = reads[i].pos;
    const double weight = indicators[i];
    for (size_t j = 0; j < reads[i].seq.length(); ++j)
      if (is_meth(reads[i], j) || un_meth(reads[i], j)) {
	meth[start + j] += weight*(is_meth(reads[i], j));
	total[start + j] += weight;
      }
  }
  for (size_t i = 0; i < n_cpgs; ++i)
    a[i] = (meth[i] + 0.5)/(total[i] + 1.0);
}


static void
maximization_step(const vector<epiread> &reads, const vector<double> &indicators,
		  vector<double> &a1, vector<double> &a2) {
  
  vector<double> inverted_indicators(indicators);
  for (size_t i = 0; i < inverted_indicators.size(); ++i)
    inverted_indicators[i] = 1.0 - inverted_indicators[i];
  
  // Fit the regular model parameters
  fit_epiallele(reads, indicators, a1);
  fit_epiallele(reads, inverted_indicators, a2);
}


static void
rescale_indicators(const double mixing, vector<double> &indic) {
  const double n_reads = indic.size();
  const double total = accumulate(indic.begin(), indic.end(), 0.0);
  const double ratio = total/n_reads;
  
  if (mixing < ratio)
    for (size_t i = 0; i < indic.size(); ++i)
      indic[i] *= (mixing/ratio);

  else {
    const double adjustment = mixing/(1.0 - ratio);
    for (size_t i = 0; i < indic.size(); ++i)
      indic[i] = 1.0 - (1.0 - indic[i])*adjustment;
  }
}


static double
expectation_maximization(const size_t max_itr, const vector<epiread> &reads, 
			 const double &mixing, vector<double> &indicators, 
			 vector<double> &a1, vector<double> &a2) {

  double prev_score = -std::numeric_limits<double>::max();
  for (size_t i = 0; i < max_itr; ++i) {
    
    const double score = expectation_step(reads, mixing, a1, a2, indicators);
    rescale_indicators(mixing, indicators);
    maximization_step(reads, indicators, a1, a2);
    
    if ((prev_score - score)/prev_score < EPIREAD_STATS_TOLERANCE)
      break;
    prev_score = score;
  }
  return prev_score;
}


double
resolve_epialleles(const size_t max_itr, const vector<epiread> &reads, 
		   vector<double> &indicators, 
		   vector<double> &a1, vector<double> &a2) {
  
  static const double MIXING_PARAMETER = 0.5; 
  
  indicators.clear();
  indicators.resize(reads.size(), 0.0);
  for (size_t i = 0; i < reads.size(); ++i) {
    const double l1 = log_likelihood(reads[i], a1);
    const double l2 = log_likelihood(reads[i], a2);
    indicators[i] = exp(l1 - log(exp(l1) + exp(l2)));
  }
  
  return expectation_maximization(max_itr, reads, MIXING_PARAMETER, 
				  indicators, a1, a2);
}

double
fit_single_epiallele(const vector<epiread> &reads, vector<double> &a) {
  assert(reads.size() > 0);
  vector<double> indicators(reads.size(), 1.0);
  fit_epiallele(reads, indicators, a);
  
  double score = 0.0;
  for (size_t i = 0; i < reads.size(); ++i) {
    score += log_likelihood(reads[i], a);
    assert(isfinite(score));
  }  
  return score;
}


double
test_asm_lrt(const size_t max_itr, const double low_prob, const double high_prob, 
	     vector<epiread> reads) {
  
  adjust_read_offsets(reads);
  const size_t n_cpgs = get_n_cpgs(reads);
  
  // try a single epi-allele
  vector<double> a0(n_cpgs, 0.5);
  const double single_score = fit_single_epiallele(reads, a0);
  
  // initialize the pair epi-alleles and indicators, and do the actual
  // computation to infer alleles
  vector<double> a1(n_cpgs, low_prob), a2(n_cpgs, high_prob), indicators;
  resolve_epialleles(max_itr, reads, indicators, a1, a2);
  
  double log_likelihood_pair = 0.0;
  for (size_t i = 0; i < reads.size(); ++i)
    log_likelihood_pair += log_likelihood(reads[i], a1, a2);
  log_likelihood_pair += reads.size()*log(0.5);
  
  const size_t df = n_cpgs;
  
  const double llr_stat = -2*(single_score - log_likelihood_pair);
  const double p_value = 1.0 - gsl_cdf_chisq_P(llr_stat, df);
  return p_value;
}

double
test_asm_lrt2(const size_t max_itr, const double low_prob, const double high_prob, 
	      vector<epiread> reads) {
  
  adjust_read_offsets(reads);
  const size_t n_cpgs = get_n_cpgs(reads);
  
  // try a single epi-allele
  vector<double> a0(n_cpgs, 0.5);
  const double single_score = fit_single_epiallele(reads, a0);
  
  // initialize the pair epi-alleles and indicators, and do the actual
  // computation to infer alleles
  vector<double> a1(n_cpgs, low_prob), a2(n_cpgs, high_prob), indicators;
  resolve_epialleles(max_itr, reads, indicators, a1, a2);
  
  const double indic_sum = 
    std::accumulate(indicators.begin(), indicators.end(), 0.0);
  
  const double log_likelihood_pair = log_likelihood(reads, indicators, a1, a2)
    - gsl_sf_lnbeta(indic_sum, reads.size() - indic_sum)
    + reads.size()*log(0.5);
  const size_t df = n_cpgs + reads.size();
  
  const double llr_stat = -2*(single_score - log_likelihood_pair);
  const double p_value = 1.0 - gsl_cdf_chisq_P(llr_stat, df);
  return p_value;
}

double
test_asm_bic(const size_t max_itr, const double low_prob, const double high_prob,
	     vector<epiread> reads) {
  
  adjust_read_offsets(reads);
  const size_t n_cpgs = get_n_cpgs(reads);
  
  // try a single epi-allele
  vector<double> a0(n_cpgs, 0.5);
  const double single_score = fit_single_epiallele(reads, a0);
  
  // initialize the pair epi-alleles and indicators, and do the actual
  // computation to infer alleles
  vector<double> a1(n_cpgs, low_prob), a2(n_cpgs, high_prob), indicators;
  resolve_epialleles(max_itr, reads, indicators, a1, a2);
  
  const double indic_sum = 
    std::accumulate(indicators.begin(), indicators.end(), 0.0);
  
  const double pair_score = log_likelihood(reads, indicators, a1, a2)
    - gsl_sf_lnbeta(indic_sum, reads.size() - indic_sum)
    + gsl_sf_lnbeta(reads.size()/2.0, reads.size()/2.0)
    + reads.size()*log(0.5);
  
  const double bic_single = n_cpgs*log(reads.size()) - 2*single_score;
  const double bic_pair = 2*n_cpgs*log(reads.size()) - 2*pair_score;
  
  return bic_pair - bic_single;
}
