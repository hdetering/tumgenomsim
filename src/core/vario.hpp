#ifndef VARIO_H
#define VARIO_H

#include "random.hpp"
#include "seqio.hpp"
#include "seqio/ChromosomeInstance.hpp"
#include "seqio/GenomeReference.hpp"
#include "seqio/GenomeInstance.hpp"
#include "stringio.hpp"
#include "evolution.hpp"
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

using seqio::GenomeReference;
using seqio::GenomeInstance;
using seqio::SeqRecord;
using seqio::SegmentCopy;
using evolution::GermlineSubstitutionModel;
using evolution::SomaticSubstitutionModel;
using evolution::SomaticCnvModel;

/**
 * Classes and methods for input/output and simulation of variants.
 */
namespace vario {

/** Variants represent variable sites in nucleotide sequences */
struct Variant
{
  std::string id;     /** unique identifier */
  std::string chr;    /** reference chromosome id */
  short reg_copy;     /** affected copy of chr region (0: original, 1: first copy, ...) */
  seqio::TCoord pos;  /** reference basepair position */
  std::vector<std::string> alleles; /** observed alleles */
  int idx_mutation;   /** reference to mutation that gave rise to this variant */
  double rel_pos;     /** relative position in genome (use for sorting) */
  bool is_somatic;    /** is this Variant somatic or germline? (different output channels) */
  bool is_het;        /** true: Variant is heterozygous; false: homozygous (only applies if not is_somatic) */
  bool is_error;      /** true: Variant due to sequencing error (only applies to read count sim) */

  Variant();
  Variant(std::string id, std::string chr, unsigned long pos);
  ~Variant();

  bool operator< (const Variant&) const; /** make variants sortable */
  /** Sort variants by absolute position in genome. */
  static std::vector<Variant> sortByPosition(const std::vector<Variant>&);
  /** Sort variants, first lexicographically by CHR, then by position in CHR. */
  static std::vector<Variant> sortByPositionLex(const std::vector<Variant>&);
  /** Sort variants by absolute position in genome, taking chomosome copies into account. */
  //static std::vector<Variant> sortByPositionPoly(const std::vector<Variant>&);
  /** Sort variants by position in reference genome. */
  static std::vector<Variant> sortByPositionRef(const std::vector<Variant>&);
  /** Returns true if this variant is a SNV, false otherwise. */
  bool isSnv();
};

/** VariantSets store a set of variants and summary statistics about them. */
struct VariantSet
{
  unsigned long num_variants = 0;
  std::vector<Variant> vec_variants; /** all variants that belong to the set */
  std::map<std::string, std::map<unsigned long, std::vector<Variant>>> map_chr2pos2var; /** variants stored by chromosome id */

  /** summary statistics */
  double mat_freqs[4][4] = {
    { 0.0, 0.0, 0.0, 0.0 },
    { 0.0, 0.0, 0.0, 0.0 },
    { 0.0, 0.0, 0.0, 0.0 },
    { 0.0, 0.0, 0.0, 0.0 }
  }; /** nucleotide substitution frequencies */

  VariantSet();
  VariantSet(std::vector<Variant> variants);
  ~VariantSet();

  /** Compound assignment VariantSets: add Variants of rhs to this VariantSet.
   *  (Does not need to be a member, but often is, to modify the private members)
   */
  VariantSet& operator+=(const VariantSet& rhs);
  /** Add two VariantSets: returns the union set.
   *  Friends defined inside class body are inline and are hidden from non-ADL lookup.
   *  Passing lhs by value helps optimize chained a+b+c. (Otherwise, both parameters
   *    may be const references)
   */
  friend VariantSet operator+(VariantSet lhs, const VariantSet& rhs) {
    lhs += rhs; // reuse compound assignment
    return lhs; // return the result by value (uses move constructor)
  }

  /** Index variants by chromosome and position. */
  long indexVariants();
  long calculateSumstats();
};

struct Genotype
{
  std::string id_variant; /** the variant this genotype refers to */
  short maternal; /** allele on maternal strand */
  short paternal; /** allele on paternal strand */
};

/** Mutations specifiy a modification of a sequence.
 *  TODO: Do I really belong here?
 */
struct Mutation
{
  /** unique identifier */
  int id;
  /** true: this Mutation is a single-nucleotide variant */
  bool is_snv;
  /** true: this Mutation is a copy-number variant */
  bool is_cnv;

  Mutation();

  // bool operator< (const Mutation&) const; /** make mutations sortable */
  // static std::vector<Mutation> sortByPosition(const std::vector<Mutation>&);
};

/** CopyNumberVariants encapsulate CNV events with all of their properties */
struct CopyNumberVariant
{
  unsigned    id;              /** unique identifier */
  bool        is_wgd;          /** true: CNV is Whole Genome Duplication */
  bool        is_deletion;     /** true: CNV is deletion event */
  bool        is_chr_wide;     /** true: event affects whole chromosome */
  bool        is_telomeric;    /** true: event coordinates include chromosome end */
  bool        is_forward;      /** true: event at 3' side of start_rel; false: at 5' end */
  double      len_rel;         /** length of affected region (fraction of chromsome length) */
  double      start_rel;       /** start position of event (fraction of chromosome length) */
  unsigned    ref_pos_begin;   /** start coordinate (in reference chr) */
  unsigned    ref_pos_end;     /** end coordinate (in reference chr) */
  std::string ref_chr;         /** affected chromosome (reference ID) */

  /** default c'tor */
  CopyNumberVariant();
};

/** Encapsulates the allele counts (reference, alternative) for a given variant and genome. */
struct VariantAlleleCount {
  //int idx_var; // this should rather be an index under which to store VariantAlleleCount elements.
  /** Total number of copies. */
  short num_tot;
  /** Number of alternative alleles. */
  short num_alt;

  /** Default c'tor. */
  VariantAlleleCount ();
};

//typedef std::map<seqio::TCoord, std::tuple<vario::Variant, short, short>> TMapPosVaf;
typedef std::map<seqio::TCoord, VariantAlleleCount> TMapPosVaf;
typedef std::map<std::string, TMapPosVaf> TMapChrPosVaf;

/** Inititalize a list of mutations, assigning a type (single-nucleotide vs. copy-number).
 *  \param vec_mutations list of (uninitialized) mutation objects
 *  \param ratio_cnv fraction of mutations that should be assigned CNV type
 *  \param rng RandomNumberGenerator object (reproducability)
 *  \return number of CNV mutations
 */
unsigned assignSomaticMutationType(
  std::vector<Mutation>& vec_mutations,
  const double ratio_cnv,
  RandomNumberGenerator& rng);

/** Read VCF file and return list of variants. */
void readVcf(
  std::string fn_vcf,
  VariantSet& variants,
  std::map<std::string, std::vector<Genotype> >& gtMatrix);
/** Read input stream with variants in VCF format and return list of variants. */
void readVcf(
  std::istream& fs_vcf,
  VariantSet& variants,
  std::map<std::string, std::vector<Genotype> >& gtMatrix);
/** Generate VCF output for a reference genome and a set of mutations.
    (multiple samples) */
void writeVcf(
  const std::vector<std::shared_ptr<SeqRecord>>& seqs,
  const std::vector<Variant>& vars,
  const std::vector<int>& id_samples,
  const std::vector<std::string>& labels,
  const std::vector<std::vector<bool> >& mutMatrix,
  std::ostream&);

/** Generate VCF output from a reference genome and a set of variants (single sample).
  * \param seqs      Reference sequences (IDs and lengths are reported in VCF header).
  * \param vars      Variants to be output.
  * \param label     Header for the genotype column.
  * \param filename  Output file name.
  */
void 
writeVcf (
  const std::vector<std::shared_ptr<SeqRecord>>& seqs,
  const std::vector<Variant>& vars,
  const std::string label,
  const std::string filename
);

/** Generate VCF output from a reference genome and a set of variants (single sample).
  * \param filename  Output file name.
  * \param genome    Genome containing ref sequences (IDs and lengths are reported in VCF header).
  * \param vars      Variants to be output.
  * \param label     Header for the genotype column.
  * \returns         Number of exported variants.
  */
unsigned 
writeVcf (
  const std::string filename,
  const GenomeReference& genome,
  const std::vector<Variant>& vars,
  const std::string label
);

/** Read mutation map (clone x mutation) from a CSV file. */
int readMutMapFromCSV(
  std::map<std::string, std::vector<bool>> &mm,
  const std::string &filename
);

/** DEPRECATED! Functionality shifted to vario::VariantStore::generateGermlineVariants
    Generate variant loci in a given genome based on evolutionary model.
    Nucleotide substitution probabilities guide selection of loci. */
// std::vector<Variant> generateGermlineVariants(
//   const int num_variants,
//   const GenomeReference& genome,
//   GermlineSubstitutionModel& model,
//   RandomNumberGenerator<>&,
//   const bool infinite_sites = false
// );

/** Generate variant loci in a given genome based on evolutionary model.
    Loci are selected randomly. */
std::vector<Variant> generateVariantsRandomPos(
  const int num_variants,
  const GenomeReference& genome,
  GermlineSubstitutionModel& model,
  RandomNumberGenerator&,
  const bool infinite_sites = false
);
/** Apply variants to a given reference sequence */
void applyVariants(
  GenomeReference&,
  const std::vector<Variant>&);
/** Apply variants to a given reference sequence */
void applyVariants(
  GenomeReference&,
  const std::vector<Variant>&,
  const std::vector<Genotype>&);

// TODO: is this functionality still useful?
/** Apply variants to a given reference sequence. streaming modified genome to output. */
// void applyVariantsStream(
//   const Genome &ref_genome,
//   const std::vector<Mutation> &mutations,
//   const std::vector<Variant> &variants,
//   std::ostream &outstream,
//   short len_line = 60);

/** Print CopyNumberVariant details (BED format). */
std::ostream& operator<<(std::ostream& lhs, const CopyNumberVariant& cnv);


} /* namespace vario */

#endif /* VARIO_H */
