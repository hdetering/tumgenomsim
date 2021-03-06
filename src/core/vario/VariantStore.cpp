#include "VariantStore.hpp"
#include <boost/container/flat_set.hpp>
using namespace std;
using boost::uuids::uuid;
using seqio::ChromosomeInstance;
using seqio::Locus;
using seqio::TCoord;

namespace vario {

unsigned
VariantStore::indexSnvs () 
{
  unsigned num_snvs = 0;
  this->map_chr_pos_snvs.clear();

  for (auto const & id_var : this->map_id_snv) {

    int id = id_var.first;
    Variant var = id_var.second;

    // if necessary, initialize pos->var map for chromosome
    if (this->map_chr_pos_snvs.find(var.chr) == this->map_chr_pos_snvs.end())
      this->map_chr_pos_snvs[var.chr] = map<TCoord, vector<int>>();

    this->map_chr_pos_snvs[var.chr][var.pos].push_back(var.idx_mutation);
    num_snvs++;
  }

  return num_snvs;
}

bool
VariantStore::generateGermlineVariants (
  const int num_variants,
  const GenomeReference& genome,
  GermlineSubstitutionModel& model,
  const double rate_hom,
  RandomNumberGenerator& rng,
  const bool inf_sites)
{
  // NOTE: germline variants carry negative indices
  int id_next = -1 * num_variants;
  //vector<Variant> variants = vector<Variant>(num_variants);
  boost::container::flat_set<int> var_pos; // keep track of variant positions
  function<double()> random_float = rng.getRandomFunctionReal(0.0, 1.0);
  function<short()> random_copy = rng.getRandomFunctionInt(short(0), short(genome.ploidy-1));
  random_selector<> selector(rng.generator); // used to pick random vector indices

  // determine base mutation probs from model (row sums)
  vector<double> p_i(4, 0);
  for (int i=0; i<4; ++i) {
    for (int j=0; j<4; ++j) {
      p_i[i] += model.Q[i][j];
    }
  }
  function<int()> random_nuc_idx = rng.getRandomIndexWeighted(p_i);

  unsigned long genome_len = genome.length; // haploid genome length
  for (int i=0; i<num_variants; ++i) {
    // pick random nucleotide bucket
    int idx_bucket = random_nuc_idx();
    // pick random position
    long nuc_pos = selector(genome.nuc_pos[idx_bucket]);
    if (inf_sites) {
      while (binary_search(var_pos.begin(), var_pos.end(), nuc_pos)) {
// TODO: check verbosity setting
fprintf(stderr, "[INFO] Infinite sites assumption: locus %ld has been mutated before, picking another one...\n", nuc_pos);
        nuc_pos = selector(genome.nuc_pos[idx_bucket]);
      }
      var_pos.insert(nuc_pos);
    }
    Locus loc = genome.getLocusByGlobalPos(nuc_pos);
    // pick new nucleotide
    short nuc_alt = evolution::MutateSite(idx_bucket, random_float, model);
    Variant var;
    var.id = stringio::format("g%d", i);
    var.is_somatic = false;
    var.is_het = ( random_float() > rate_hom );
    var.chr = loc.id_ref;
    //var.chr_copy = random_copy(); // TODO: deprecated!
    //var.rel_pos = double(nuc_pos-(var.chr_copy*genome_len))/genome_len;
    var.rel_pos = double(nuc_pos)/genome_len;
    //var.reg_copy = 0; // TODO: deprecated!
    var.pos = loc.start;
    var.alleles.push_back(string(1, seqio::idx2nuc(idx_bucket)));
    var.alleles.push_back(string(1, seqio::idx2nuc(nuc_alt)));
    var.idx_mutation = id_next;

    this->map_id_snv[id_next] = var;
    id_next++;
    // variants[i] = var;
  }

  // sanity check: correct number of variants?
  assert ( id_next == 0 );
  return true;
}

/** Generate variant loci in a given genome based on somatic mutation model. */
bool
VariantStore::generateSomaticVariants(
  const vector<Mutation>& vec_mutations,
  const GenomeReference& genome,
  SomaticSubstitutionModel& model_snv,
  SomaticCnvModel& model_cnv,
  RandomNumberGenerator& rng,
  const bool inf_sites)
{
  // SNV events
  //------------
  vector<Variant> variants;
  // keep track of variant positions (ISM)
  boost::container::flat_set<int> var_pos;
  // random function, returns substitution index
  function<int()> r_idx_sub = rng.getRandomIndexWeighted(model_snv.m_weight);
  // TODO: deprecated!
  function<short()> random_copy = rng.getRandomFunctionInt(short(0), short(1)); // was genome.ploidy-1
  random_selector<> selector(rng.generator); // used to pick random vector indices
  unsigned long genome_len = genome.length; // haploid genome length
  //------------

  // CNV events
  //------------
  // random function, selects CNV event type
  function<int()> r_idx_cnv_type = rng.getRandomIndexWeighted({
    model_cnv.rate_wgd, // 0
    model_cnv.rate_chr, // 1
    model_cnv.rate_arm, // 2
    model_cnv.rate_tel, // 3
    model_cnv.rate_foc  // 4
  });
  // random function, selects affected reference chromosome (weighted by size)
  function<int()> r_idx_chr = rng.getRandomIndexWeighted(genome.vec_chr_len);
  // random function, selects CNV event length (relative to CHR len)
  // random function, gives a random number between 0 and 1.
  function<double()> r_prob = rng.getRandomFunctionReal(0.0, 1.0);
  //------------

  for (auto m : vec_mutations) {
    if (m.is_snv) {
      // pick random ref->alt substitution
      int i_sub = r_idx_sub();
      string ref_site = model_snv.m_site[i_sub];
      string alt_nuc = model_snv.m_alt[i_sub];
      string ref_nuc = ref_site.substr(1, 1);
      // pick random position (+1 b/c second nucleotide in 3-mer is mutated)
      long nuc_pos = selector(genome.map_3mer_pos.at(ref_site)) + 1;
      if (inf_sites) {
        while (binary_search(var_pos.begin(), var_pos.end(), nuc_pos)) {
          // TODO: check verbosity setting
          fprintf(stderr, "[INFO] Infinite sites model: locus %ld has been mutated before, picking another one...\n", nuc_pos);
          nuc_pos = selector(genome.map_3mer_pos.at(ref_site)) + 1;
        }
        var_pos.insert(nuc_pos);
      }
      Locus loc = genome.getLocusByGlobalPos(nuc_pos);
      // TODO: identify available segment copies in GenomeInstance, choose one

      // init new Variant
      Variant var;
      var.id = stringio::format("s%d", m.id);
      var.chr = loc.id_ref;
      var.rel_pos = double(nuc_pos)/genome_len;
      var.pos = loc.start;
      var.alleles.push_back(ref_nuc);
      var.alleles.push_back(alt_nuc);
      var.idx_mutation = m.id;
      var.is_somatic = true;
      this->map_id_snv[m.id] = var;
    }
    else { // CNV event
      CopyNumberVariant cnv;
      cnv.id = m.id;
      // pick event type
      int idx_cnv_type = r_idx_cnv_type();
      // pick affected chromosome
      int idx_cnv_chr = r_idx_chr();
      cnv.ref_chr = genome.vec_chr_id[idx_cnv_chr];
      // pick length of affected region
      double min_len_rel = double(model_cnv.len_min) / genome.vec_chr_len[idx_cnv_chr];
      double cnv_len_rel = rng.getRandomParetoBounded(model_cnv.len_exp, min_len_rel, 1.0);
      cnv.len_rel = cnv_len_rel;
      // pick a direction (i.e., insert up- vs. down-stream from pos_rel)
      cnv.is_forward = (r_prob() <= 0.5);
      // pick insertion vs. deletion
      cnv.is_deletion = (r_prob() > model_cnv.gain_prob);

      // set event-specific properties
      // NOTE: some properties assigned above may be ignored depending on event-type
      switch (idx_cnv_type) {
        case 0: // whole genome duplication
          cnv.is_wgd = true;
          break;
        case 1: // chromosome-level event
          cnv.is_chr_wide = true;
          break;
        case 2: // chromosome arm-level event
          // TODO: set pos_rel to centromere location
          cnv.start_rel = 0.5;
          cnv.len_rel = 0.5;
          break;
        case 3: // telomere-bounded event
          cnv.is_telomeric = true;
          cnv.start_rel = cnv.is_forward ? 1-cnv.len_rel : cnv.len_rel;
          break;
        case 4: // focal event
          // pick random breakpoint location
          cnv.start_rel = r_prob();
          // make sure that chromosome ends are respected
          if (cnv.is_forward) {
            cnv.start_rel = min(cnv.start_rel, 1.0-cnv.len_rel);
          } else {
            cnv.start_rel = max(cnv.start_rel, cnv.len_rel);
          }
          break;
        default: // do nothing
          break;
      }

      this->map_id_cnv[m.id] = cnv;
    }
  }

  // index SNVs by chromosome and ref position
  this->indexSnvs();

  return true;
}

bool 
VariantStore::applyGermlineVariants (
  GenomeInstance& genome,
  RandomNumberGenerator& rng
)
{
  random_selector<> selector(rng.generator); // used to pick random SegmentCopy

  // loop over variants
  for (auto const & id_snv : map_id_snv) {
    int id = id_snv.first;
    Variant var = id_snv.second;

    // skip somatic variants
    if (var.is_somatic) continue;

    // get available SegmentCopies from genome
    string id_chr = var.chr;
    TCoord ref_pos = var.pos;
    vector<SegmentCopy> vec_seg_avail = genome.getSegmentCopiesAt(id_chr, ref_pos);

    // SegmentCopies that will carry the variant
    vector<SegmentCopy> vec_seg_mut;
    // homozygous variants are introduced into all SegmentCopies, heterozygous ones into random one
    if ( var.is_het ) {
      // pick random SegmentCopy to mutate
      vec_seg_mut.push_back( selector(vec_seg_avail) );
    } else {
      // mark all SegmentCopies for mutation
      vec_seg_mut = vec_seg_avail;
    }
    
    // initialize or append to Variant vector of segment copies
    for ( SegmentCopy sc : vec_seg_mut ) {
      if ( map_seg_vars.count(sc.id) == 0 ) {
        map_seg_vars[sc.id] = { id };
      } else {
        map_seg_vars[sc.id].push_back(id);
      }
    }
  }

  return true;
}

void 
VariantStore::applyMutation (
  Mutation mut,
  GenomeInstance& genome,
  RandomNumberGenerator& rng
)
{
  // perform some sanity checks
  assert( mut.is_snv != mut.is_cnv );
  assert( !mut.is_snv || this->map_id_snv.count(mut.id)>0 );
  assert( !mut.is_cnv || this->map_id_cnv.count(mut.id)>0 );

  random_selector<> selector(rng.generator); // used to pick random SegmentCopy

  if ( mut.is_snv ) { // SNV mutation
    Variant snv = this->map_id_snv[mut.id];
    // get available SegmentCopies
    vector<SegmentCopy> seg_targets = genome.getSegmentCopiesAt(snv.chr, snv.pos);
    if ( seg_targets.size() == 0 ) {
      fprintf(stderr, "[INFO] (VariantStore::applyMutation) SNV '%d' masked (no locus '%s:%lu').\n", mut.id, snv.chr.c_str(), snv.pos);
      return;
    }
    SegmentCopy sc = selector(seg_targets);
    // initialize or append to Variant vector of SegmentCopy
    if ( map_seg_vars.count(sc.id) == 0 ) {
      map_seg_vars[sc.id] = { mut.id };
    } else {
      map_seg_vars[sc.id].push_back(mut.id);
    }
  }
  else { // CNV mutation
    CopyNumberVariant cnv = this->map_id_cnv[mut.id];
    // track modifications to SegmentCopies
    vector<seqio::seg_mod_t> vec_seg_mod;

    if (cnv.is_wgd) { // Whole Genome Duplication
      genome.duplicate(vec_seg_mod);
      this->transferMutations(vec_seg_mod);
    }
    else { // Not a WGD, chromosome region is affected
      // pick a ChromsomeInstance randomly (weighted by chromosome lengths)
      string id_chr = cnv.ref_chr;
      assert( genome.map_id_chr.find(id_chr) != genome.map_id_chr.end() );
      vector<unsigned long> chr_len;
      for (auto const & chr_inst : genome.map_id_chr[id_chr]) {
        chr_len.push_back(chr_inst->length);
      }
      function<int()> r_idx_chr = rng.getRandomIndexWeighted(chr_len);
      int idx_chr = r_idx_chr();
      shared_ptr<ChromosomeInstance> sp_chr = genome.map_id_chr[id_chr][idx_chr];

      if (cnv.is_chr_wide) { // whole-chromosome gain/loss
        if (cnv.is_deletion) { // delete current chromosome
          genome.deleteChromosome(sp_chr, id_chr);
        } else {
          vector<seqio::seg_mod_t> vec_seg_mod;
          shared_ptr<ChromosomeInstance> sp_chr_new(new ChromosomeInstance());
          sp_chr_new->copy(sp_chr, vec_seg_mod);
          this->transferMutations(vec_seg_mod);
        }
      } else if (cnv.is_deletion) { // delete a genomic region
        vec_seg_mod = sp_chr->deleteRegion(cnv.start_rel, cnv.len_rel, cnv.is_forward, cnv.is_telomeric);
        this->transferMutations(vec_seg_mod);
      } else { // amplify a genomic region
        vec_seg_mod = sp_chr->amplifyRegion(cnv.start_rel, cnv.len_rel, cnv.is_forward, cnv.is_telomeric);
        this->transferMutations(vec_seg_mod);
      }
// sanity check: does chromosome length equal length of SegmentCopies?
ulong len = 0;
vector<ulong> lens;
for (auto s : sp_chr->lst_segments) {
  lens.push_back(s.ref_end - s.ref_start);
  len += (s.ref_end - s.ref_start);
}
if (len != sp_chr->length) {
  cerr << "### Lengths are off!! ###";
}
    }
  }
}

void VariantStore::transferMutations(vector<seqio::seg_mod_t> vec_seg_mod) {
  uuid seg_new_id;
  uuid seg_old_id;
  seqio::TCoord seg_old_start;
  seqio::TCoord seg_old_end;

  // loop over SegmentCopy modifications
  for (auto const & tpl_seg_mod : vec_seg_mod) {
    // each modification carries information about:
    // - newly created SegmentCopy
    // - existing SegmentCopy it was copied from
    // - interval (start, end) the new SegmentCopy originates from
    tie(seg_new_id, seg_old_id, seg_old_start, seg_old_end) = tpl_seg_mod;

    // transfer variants associated with the copied region within old SegmentCopy
    vector<int> seg_new_vars;
    auto it_vars = this->map_seg_vars.find(seg_old_id);
    if (it_vars == this->map_seg_vars.end())
      continue;
    for (auto const & id_snv : it_vars->second) {
      Variant snv = this->map_id_snv[id_snv];
      if (snv.pos >= seg_old_start && snv.pos < seg_old_end) {
        seg_new_vars.push_back(id_snv);
      }
    }
    if (seg_new_vars.size() > 0) {
      this->map_seg_vars[seg_new_id] = seg_new_vars;
    }
  }
}

vector<Variant>
VariantStore::getGermlineSnvVector ()
{
  vector<Variant> variants;
  for (auto const & kv : this->map_id_snv) {
    Variant var = kv.second;
    if ( !var.is_somatic )
      variants.push_back(var);
  }

  return variants;
}


vector<Variant> 
VariantStore::getSomaticSnvVector () 
{
  vector<Variant> variants;
  for (auto const & kv : this->map_id_snv) {
    Variant var = kv.second;
    if ( var.is_somatic )
      variants.push_back(var);
  }

  return variants;
}

int
VariantStore::getSnvsForSegmentCopy (
  map<seqio::TCoord, vector<Variant>>& map_vars,
  const boost::uuids::uuid id_seg
) const
{
  int n_vars = 0;
  map_vars.clear();

  auto it_seg_vars = this->map_seg_vars.find(id_seg);
  if (it_seg_vars != this->map_seg_vars.end()) {
    for (int id_var : it_seg_vars->second) {
      Variant var = this->map_id_snv.at(id_var);
      if (map_vars.count(var.pos) == 0)
        map_vars[var.pos] = vector<Variant>();
      map_vars[var.pos].push_back(var);

        
      n_vars++;
    }
  }
  
  return n_vars;
}

int
VariantStore::getSnvsForSegmentCopy (
  map<seqio::TCoord, vector<Variant>>& map_vars,
  const boost::uuids::uuid id_seg,
  const TCoord pos_start,
  const TCoord pos_end
) const
{
  int n_vars = 0;
  map_vars.clear();

  auto it_seg_vars = this->map_seg_vars.find(id_seg);
  if (it_seg_vars != this->map_seg_vars.end()) {
    for (int id_var : it_seg_vars->second) {
      Variant var = this->map_id_snv.at(id_var);
      if ( var.pos>=pos_start && var.pos<=pos_end ) {
        if (map_vars.count(var.pos) == 0)
          map_vars[var.pos] = vector<Variant>();
        map_vars[var.pos].push_back(var);
      }
        
      n_vars++;
    }
  }
  
  return n_vars;
}

unsigned 
VariantStore::writeGermlineSnvsToVcf (
  const std::string filename,
  const GenomeReference& genome
)
{
  unsigned n_vars = 0;

  // retrieve germline SNVs
  vector<Variant> vec_var;
  for (auto const & id_snv : map_id_snv) {

    int id = id_snv.first;
    Variant var = id_snv.second;

    if (!var.is_somatic)
      vec_var.push_back(var);
  }

  n_vars = writeVcf(filename, genome, vec_var, "germline");
  return n_vars;
}

unsigned 
VariantStore::writeCnvsToFile (
  string filename
) 
{
  ofstream f_out;
  f_out.open(filename);
  f_out << "id_cnv\tclass\tscope\tchr\tstart_rel\tdirection\tlen_rel\n";
  for (auto kv : this->map_id_cnv) {
    f_out << kv.second;
  }
  f_out.close();
}

} // namespace vario