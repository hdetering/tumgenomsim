#ifndef VARIANTSTORE_H
#define VARIANTSTORE_H

#include "../vario.hpp"

namespace vario {

/** Keeps somatic variants (SNVs, CNVs) as well as their association to
 *  genomic segment copies.
 */
struct VariantStore
{
  /** map of single-nucleotide variants */
  std::map<int, Variant> map_id_snv;
  /** map of somatic copy-number variants */
  std::map<int, CopyNumberVariant> map_id_cnv;
  /** remember SNVs affecting each SegmentCopy */
  std::map<boost::uuids::uuid, std::vector<int>> map_seg_vars;
  /** index SNVs by chromosome and ref position for fast lookup during spike-in. */
  std::map<std::string, std::map<seqio::TCoord, std::vector<int>>> map_chr_pos_snvs;

  /** Index variants by chromosome and position. 
   *  \returns Number of indexed SNVs.
   */
  unsigned indexSnvs ();

  /** Get vector of germline SNVs. */
  std::vector<Variant> getGermlineSnvVector ();

  /** Get vector of somatic SNVs. */
  std::vector<Variant> getSomaticSnvVector ();

  /** Get VariantSet of SNVs. */
  VariantSet getSnvSet ();

  /** Get Variants associated with SegmentCopy. 
    * \param map_vars  Output param: Variants indexed by position.
    * \param id_seg    SegmentCopy id for which to retrieve variants.
    * \returns         Number of variants returned.
    */
  int
  getSnvsForSegmentCopy (
    std::map<seqio::TCoord, std::vector<Variant>>& map_vars,
    const boost::uuids::uuid id_seg
  ) const;

  /** Get Variants associated with SegmentCopy between given positions. 
    * \param map_vars   Output param: Variants indexed by position.
    * \param id_seg     SegmentCopy id for which to retrieve variants.
    * \param pos_start  Left-most position for variants to be output.
    * \param pos_end    Right-most position for variants to be output.
    * \returns          Number of variants returned.
    */
  int
  getSnvsForSegmentCopy (
    std::map<seqio::TCoord, std::vector<Variant>>& map_vars,
    const boost::uuids::uuid id_seg,
    const seqio::TCoord pos_start,
    const seqio::TCoord pos_end
  ) const;

  /** Import germline variants from outside source.
   *  \returns true on success, false on error.
   */
  bool
  importGermlineVariants (
    VariantSet variants
  );

  /** Generate variants for a reference genome based on an evolutionary model.
   *  Nucleotide substitution probabilities guide selection of loci.
   *  \param num_variants Number of germline variants to generate.
   *  \param genome_ref   Reference genome containing DNA sequences to be mutated.
   *  \param model        Sequence subtitution model to generate mutations.
   *  \param rate_hom     Ratio of homozygous germline variants to generate.
   *  \param rng          Random number generator.
   *  \param inf_sites    Should infinite sites assumption be enforced?
   *  \returns            True on success, false on error.
   */
  bool
  generateGermlineVariants (
    const int num_variants,
    const GenomeReference& genome_ref,
    GermlineSubstitutionModel& model,
    const double rate_hom,
    RandomNumberGenerator& rng,
    const bool inf_sites = true
  );

  /** Generate variant loci in a given genome based on somatic mutation model.
      Use context-dependent mutation signature to select loci. */
  bool
  generateSomaticVariants (
    const std::vector<Mutation>& vec_mutations,
    const GenomeReference& genome,
    SomaticSubstitutionModel& model_snv,
    SomaticCnvModel& model_cnv,
    RandomNumberGenerator& rng,
    const bool infinite_sites = false
  );

  /** Loop over variants and for each germline variant, pick affected segment copies in genome instance.
   *  \param genome  Genome instance to which to apply germline variants.
   *  \param rng     Random number generator (used to pick segment copies to mutate).
   *  \returns       true on success, false on error.
   */
  bool
  applyGermlineVariants (
    GenomeInstance& genome,
    RandomNumberGenerator& rng
  );

  /** Apply a mutation to a GenomeInstance.
   *  - SNV: A SegmentCopy will be chosen from the affected ChromosomeInstance.
   *  - CNV (gain): new SequenceCopies will be introduced
   *  - CNV (loss): existing SequenceCopies will be split
   */
  void applyMutation(Mutation m, GenomeInstance& g, RandomNumberGenerator& r);

  /** Transfer mutations from existing SegmentCopies to new ones. */
  void transferMutations(std::vector<seqio::seg_mod_t> vec_seg_mod);

  /** Write somatic SNVs to VCF file.
   *  \param filename  Output file name.
   *  \param genome    Reference genome (contains lengths of ref seqs).
   *  \returns         Number of exported variants.
   */
  unsigned 
  writeGermlineSnvsToVcf (
    const std::string filename,
    const GenomeReference& genome
  );

  /** Write CNVs to output file.
   *  \param filename Output file name.
   */
  unsigned writeCnvsToFile(std::string filename);
};

} /* namespace vario */

#endif /* VARIANTSTORE_H */