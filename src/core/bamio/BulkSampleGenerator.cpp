#include "BulkSampleGenerator.hpp"
#include <chrono>
#include <tuple>
using namespace std;
using namespace boost::filesystem;
using namespace seqan;
using boost::icl::interval_map;
using boost::icl::interval;
using seqio::GenomeInstance;
using seqio::GenomeReference;
using seqio::TCoord;
using seqio::TRegion;
using stringio::format;

namespace bamio {

BulkSampleGenerator::BulkSampleGenerator ()
: has_samples(false),
  has_refseqs(false),
  has_clone_genomes(false),
  has_cn_states(false),
  m_ref_len(0)
{}

bool
BulkSampleGenerator::initSamples (
  const map<string, map<string, double>> mtx_sample_clone_w
) 
{
  for (auto smp_clone_w : mtx_sample_clone_w) {
    string              id_smp  = smp_clone_w.first;
    map<string, double> clone_w = smp_clone_w.second;

    BulkSample sample(id_smp, clone_w);
    this->m_samples[id_smp] = sample;
  } 

  this->has_samples = true;

  return true;
}

void
BulkSampleGenerator::initRefSeqs (
  const seqio::GenomeReference& ref_genome
)
{
  for (auto& ref_chr : ref_genome.chromosomes) {
    string id = ref_chr.first;
    shared_ptr<seqio::ChromosomeReference> chr = ref_chr.second;
    this->m_map_ref_len[id] = chr->length;
    this->m_ref_len += chr->length;
  }

  this->m_ref_genome = make_shared<seqio::GenomeReference>(ref_genome);
  this->has_refseqs = true;
}

void
BulkSampleGenerator::generateBulkSamples (
  const map<string, map<string, double>> mtx_sample_clone_w,
  const vario::VariantStore var_store,
  const path path_fasta,
  const path path_bam,
  const path path_bed,
  const path path_log,
  const double seq_coverage,
  const double seq_rc_error,
  const double seq_rc_disp,
  const int    seq_rc_min,
  const bool seq_read_gen,
  const bool seq_use_vaf,
  const unsigned seq_read_len,
  const unsigned seq_frag_len_mean,
  const unsigned seq_frag_len_sd,
  const std::string art_bin,
  RandomNumberGenerator& rng
)
{
  // for (auto const sample_weights : mtx_sample_clone_w) {

  //   string lbl_sample = sample_weights.first;
  //   map<string, double> w = sample_weights.second;

  //   // initialize new bulk sample
  //   BulkSample sample(lbl_sample, w);
  //   this->m_samples[lbl_sample] = sample;
  #pragma omp parallel
  {
  #pragma omp single
  {
  for (auto lbl_smp : this->m_samples) {
    #pragma omp task
    {
      int ithread = omp_get_thread_num();
      int nthreads = omp_get_num_threads();

    string lbl_sample = lbl_smp.first;
    BulkSample sample = lbl_smp.second;
#ifndef NDEBUG
    fprintf(stderr, "lbl_sample: %s (thread %d of %d)\n", lbl_sample.c_str(), ithread, nthreads);
#endif
    // initialize expected SNV allele frequencies
    map<string, double> w = sample.m_clone_weight;
    sample.initAlleleCounts(w, var_store, m_map_clone_chr_seg);

    // output expected read counts to BED file.
    path fn_vaf = path_bed / format("%s.vaf.bed", lbl_sample.c_str()) ;
    std::ofstream ofs_vaf(fn_vaf.string(), std::ofstream::out);
    writeExpectedReadCounts(ofs_vaf, seq_coverage, var_store, sample.m_map_snv_vaf);
    ofs_vaf.close();

    fprintf(stdout, "Generating bulk sample '%s'\n", lbl_sample.c_str());
    
    if ( seq_read_gen ) { // generate sequencing reads      
      // generate read groups to identify clones in samples
      vector<string> vec_clone_lbl = sample.getCloneLabels();
      vector<seqan::BamHeaderRecord> vec_rg;
      generateReadGroups(vec_rg, lbl_sample, vec_clone_lbl, "Illumina", "HiSeq2500");

      // initialize ART wrapper
      bamio::ArtWrapper art(art_bin);
      art.read_len = seq_read_len;
      art.frag_len_mean = seq_frag_len_mean;
      art.frag_len_sd = seq_frag_len_sd;
      art.out_sam = true;

      generateBulkSeqReads(path_fasta, path_bam, path_log, lbl_sample, w, seq_coverage, art, rng);
      mergeBulkSeqReads(path_bam, lbl_sample, vec_rg, var_store, seq_use_vaf, rng);
    } 
    else { // generate read counts
      generateReadCounts(
        path_bam, 
        lbl_sample, 
        seq_coverage, 
        seq_rc_disp,
        seq_rc_error,
        seq_rc_min, 
        var_store,
        rng
      );
    }
    } // pragma omp task
  }
  } // pragma omp single
  } // pragma omp parallel
}

/** 
 * TODO: how are read counts generated for colliding variants (at same locus) ???
 * */
bool
BulkSampleGenerator::generateReadCounts (
  const path path_out,
  const string lbl_sample,
  const double seq_coverage,
  const double seq_disp,
  const double seq_error,
  const int    seq_min_rc,
  const vario::VariantStore& var_store,
  RandomNumberGenerator& rng
) 
{
  // sanity checks
  assert ( this->has_refseqs );
  assert ( this->has_samples );
  assert ( this->has_cn_states );

  // store read counts per chromosome, position and base
  map<string, map<TCoord, map<string, int>>> map_chr_pos_base_rc;

  // store true variants per chromosome, position and variant
  // (colliding variants identified by their int index)
  //map<string, map<TCoord, map<int, pair<int, int>>>> map_chr_pos_var_rc;
  map<string, map<TCoord, vector<string>>> map_chr_pos_var;

  // get sample we are dealing with
  BulkSample sample = this->m_samples[lbl_sample];

  // calculate expected coverage per single copy
  double cvg_per_cpy = double(seq_coverage) * m_ref_len / sample.genome_len_abs;

  // STEP 1: generate read counts for true variants
  //---------------------------------------------------------------------------
  for (auto snv_vaf : sample.m_map_snv_vaf) {
    int id_snv = snv_vaf.first;
    double vaf = snv_vaf.second;
    Variant var = var_store.map_id_snv.at(id_snv);

    // calculate copy number-adjusted expected coverage
    double cn_seg;
    TCoord seg_len;
    sample.getTotalCopyNumberAt(var.chr, var.pos, cn_seg, seg_len);
    double cvg_exp = cn_seg * cvg_per_cpy;

    // sample total read count from Negative Binomial distribution
    int rc_tot = rng.getRandomNegativeBinomial(cvg_exp, seq_disp);

    // sample alternative read count from Binomial distribution
    int rc_alt = rng.getRandomFunctionBinomial(rc_tot, vaf)();

    // store true variants
    if ( map_chr_pos_var.count(var.chr) == 0 ) {
      //map_chr_pos_var_rc[var.chr] = map<TCoord, map<int, pair<int, int>>>();
      map_chr_pos_var[var.chr] = map<TCoord, vector<string>>();
    }
    if ( map_chr_pos_var[var.chr].count(var.pos) == 0 ) {
      //map_chr_pos_var_rc[var.chr][var.pos] = map<int, pair<int, int>>();
      map_chr_pos_var[var.chr][var.pos] = vector<string>();
    }
    //map_chr_pos_var_rc[var.chr][var.pos][var.idx_mutation] = make_pair(rc_tot, rc_alt);
    map_chr_pos_var[var.chr][var.pos].push_back(var.id);

    // store per-base read counts
    if ( map_chr_pos_base_rc.count(var.chr) == 0 ) {
      map_chr_pos_base_rc[var.chr] = map<TCoord, map<string, int>>();
    }
    if ( map_chr_pos_base_rc[var.chr].count(var.pos) > 0 ) {
      // TODO: How to handle this case consistently?
fprintf(stderr, "### BulkSampleGenerator::GenerateReadCounts(): colliding variants at '%s_%lu'!", var.chr.c_str(), var.pos);      
    }
    map_chr_pos_base_rc[var.chr][var.pos] = map<string, int>();
    map_chr_pos_base_rc[var.chr][var.pos][var.alleles[0]] = rc_tot - rc_alt;
    map_chr_pos_base_rc[var.chr][var.pos][var.alleles[1]] = rc_alt;
  } 

  // STEP 2: introduce sequencing errors (incl. FP variant loci)
  //---------------------------------------------------------------------------
  
  // determine expected number of seq errors
  unsigned n_err_exp = m_ref_len * seq_error * seq_coverage;
  // sample number of seq errors to introduce
  unsigned n_err = rng.getRandomFunctionPoisson(n_err_exp)();
  // random function to sample relative genome positions
  function<double()> r_pos_rel = rng.getRandomFunctionReal(0.0, 1.0);

  // introduce sequencing errors at random genomic positions
  function<TCoord()> r_unif = rng.getRandomFunctionInt(TCoord(0), m_ref_len);
  for (unsigned i=0; i<n_err; i++) {
    // pick random reference position
    TCoord pos_abs = r_pos_rel() * this->m_ref_len;
    // identify chromosome and bp position
    seqio::Locus loc_err = this->m_ref_genome->getLocusByGlobalPos(pos_abs);
    string chr_err = loc_err.id_ref;
    TCoord pos_err = loc_err.start;
    
    // init error locus if necessary
    if ( map_chr_pos_base_rc.count(chr_err) == 0 ) {
      map_chr_pos_base_rc[chr_err] = map<TCoord, map<string, int>>();
    }
    if ( map_chr_pos_base_rc[chr_err].count(pos_err) == 0 ) {
      // get reference nucleotide for error position
      string ref_nuc;
      this->m_ref_genome->getSequence(chr_err, pos_err, pos_err+1, ref_nuc);
      // calculate copy number-adjusted expected coverage
      double cn_seg;
      TCoord seg_len;
      sample.getTotalCopyNumberAt(chr_err, pos_err, cn_seg, seg_len);
      double cvg_exp = cn_seg * cvg_per_cpy;
      // sample total read count from Negative Binomial distribution
      int rc_tot = rng.getRandomNegativeBinomial(cvg_exp, seq_disp);

      map_chr_pos_base_rc[chr_err][pos_err][ref_nuc] = rc_tot;
    }

    // change one nucleotide due to error

    // determine alleles present at locus and their read counts
    vector<string> vec_alleles;
    vector<int> vec_rc;
    for (auto kv : map_chr_pos_base_rc[chr_err][pos_err]) {
      vec_alleles.push_back(kv.first);
      vec_rc.push_back(kv.second);
    }
    // pick allele to be affected by read error
    int idx_allele = rng.getRandomIndexWeighted(vec_rc)();
    string allele_old = vec_alleles[idx_allele];
    // error means read count is reduced by one
    map_chr_pos_base_rc[chr_err][pos_err][allele_old] -= 1;

    // determine allele that arises due to error
    short nuc_old = seqio::nuc2idx(allele_old[0]);
    short shift = rng.getRandomFunctionInt(1, 3)();
    string nuc_err( 1, seqio::idx2nuc( (nuc_old + shift) % 4 ) );

    // update read count for error allele
    if ( map_chr_pos_base_rc[chr_err][pos_err].count(nuc_err) == 0 ) {
      map_chr_pos_base_rc[chr_err][pos_err][nuc_err] = 1;
    }
    else { // existing allele increases in read count
      map_chr_pos_base_rc[chr_err][pos_err][nuc_err] += 1;
    }
  }

  // OUTPUT: Write read counts to file
  //---------------------------------------------------------------------------
  
  // create output file
  string fn_out = (path_out / format("%s.rc.vcf", lbl_sample.c_str())).string();
  writeReadCountsVcf(fn_out, map_chr_pos_base_rc, map_chr_pos_var, seq_min_rc);

  return true;
}

bool
BulkSampleGenerator::writeReadCountsVcf (
  const string filename,
  const map<string, map<TCoord, map<string, int>>> map_chr_pos_nuc_rc,
  const map<string, map<TCoord, vector<string>>> map_chr_pos_var,
  const int min_rc
) const
{
  std::ofstream ofs(filename);

  // write header
  ofs << "##fileformat=VCFv4.1" << endl;
  ofs << "##INFO=<ID=DP,Number=1,Type=Integer,Description=\"Total Depth\">" << endl;
  ofs << "##INFO=<ID=AC,Number=A,Type=Integer,Description=\"Allele Count\">" << endl;
  //ofs << "##FORMAT=<ID=DP,Number=1,Type=Integer,Description=\"Read Depth\">" << endl;
  ofs << "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO" << endl;

  //string fmt = "DP:AC";

  // write variable positions
  for (auto & chr_rc : map_chr_pos_nuc_rc) {
    string chr = chr_rc.first;
    for (auto & pos_rc : chr_rc.second) {
      TCoord pos = pos_rc.first;
      // determine REF allele
      string ref;
      this->m_ref_genome->getSequence(chr, pos, pos+1, ref);

      int depth = 0;
      string alt, ac;
      bool do_write_line = false;
      bool is_first_allele = true;
      for (auto & nuc_rc : pos_rc.second) {
        string nuc = nuc_rc.first;
        int rc = nuc_rc.second;
        depth += rc;

        // check if ALT allele
        if ( nuc != ref ) {
          alt += (is_first_allele ? "" : ",") + nuc;
          ac  += (is_first_allele ? "" : ",") + to_string(rc);
          is_first_allele = false;

          // check if ALT allele exceeds minimum read count threshold
          if ( rc >= min_rc ) do_write_line = true;
        }
      }

      // check if VCF line is to be exported
      if ( !do_write_line ) continue;

      // determine IDs of true variants at locus, if any
      string id_vars = ".";
      if ( map_chr_pos_var.count(chr) > 0 && 
           map_chr_pos_var.at(chr).count(pos) > 0 ) {
        id_vars = map_chr_pos_var.at(chr).at(pos)[0];
        // there could be >1 variant at locus (violation of infinite sites)
        for (auto i=1; i<map_chr_pos_var.at(chr).at(pos).size(); i++) {
          id_vars += "," + map_chr_pos_var.at(chr).at(pos)[i];
        }
      }

      string info = format("DP=%d;AC=%s", depth, ac.c_str());
      // #CHROM  POS  ID  REF  ALT  QUAL  FILTER  INFO
      ofs << format("%s\t%lu\t%s\t%s\t%s\t.\tPASS\t%s\n", 
                    chr.c_str(), (pos+1), id_vars.c_str(), 
                    ref.c_str(), alt.c_str(), info.c_str());
    }
  }

  ofs.close();
  return true;
}


void
BulkSampleGenerator::generateBulkSeqReads (
  const path path_fasta,
  const path path_bam,
  const path path_log,
  const string lbl_sample,
  const map<string, double> map_clone_weight,
  const double cvg_total,
  ArtWrapper& art,
  RandomNumberGenerator& rng
) 
{
  // sanity checks
  // have reference sequences been initialised?
  assert ( this->has_refseqs );
  // have clone genomes been indexed?
  assert ( this->has_clone_genomes );

  // calculate total number of reads
  unsigned long long len_ref = 0;
  for (auto const & kv : m_map_ref_len)
    len_ref += kv.second;
  unsigned long n_reads_tot = cvg_total * len_ref / art.read_len;
  // calculate number of reads for each clone (weight * reads_total)
  map<string, long> map_clone_reads;
  for (auto const clone_w : map_clone_weight) {
    string lbl = clone_w.first;
    double w = clone_w.second;
    map_clone_reads[lbl] = floor(w * n_reads_tot);
  }

  // calculate relative coverage for clones (weight * cvg_total)
  // map<string, double> map_clone_cvg;
  // for (auto i=0; i<vec_clone_lbl.size(); i++) {
  //   string lbl = vec_clone_lbl[i];
  //   double cvg = vec_weight[i] * cvg_total;
  //   map_clone_cvg[lbl] = cvg;
  // }

  // for(auto& entry: directory_iterator(path_fasta)) {
  for(auto const & fasta_len : this->m_map_fasta_len) {
    path path_fa = fasta_len.first;
    unsigned long long seq_len = fasta_len.second;

    //string fn_pfx = basename(entry);
    string fn_pfx = basename(path_fa);
    // expected filename pattern: <clone_id>.<coverage>.fa
    vector<string> fn_parts = stringio::split(fn_pfx, '.');
    if ( fn_parts.size() < 2 ) {
      fprintf(stderr, "[ERROR] (BulkSampleGenerator::generateBulkSeqReads)\n");
      fprintf(stderr, "        expected filename pattern: <clone_id>.<coverage>.fa\n");
      fprintf(stderr, "        found: '%s'\n", fn_pfx.c_str());
      continue;
    }

    string id_clone = fn_parts[0];
    int copy_number = stoi(fn_parts[1]);

    // sanity check: clone label valid?
    assert ( map_clone_reads.count(id_clone) == 1 );

    // calculate number of reads to be sampled from this segment
    // (total number of reads) * (fraction of genome contained)
    unsigned long long genome_len = this->m_map_clone_len[id_clone];
    double seq_frac = double(seq_len) * copy_number / genome_len;
    unsigned long n_reads = map_clone_reads[id_clone] * seq_frac;
    double cvg = double(n_reads) / seq_len * art.read_len;
    //double cvg = map_clone_cvg[id_clone] / 2 * copy_number;

    // generate random seed for sequencing sim
    unsigned long rnd_seed = rng.getRandomFunctionInt(0, numeric_limits<int>::max())();
    // construct log file path
    path fn_log = path_log / (lbl_sample+"."+fn_pfx+".art.log");

    if ( cvg > 0.0 ) {
#ifndef NDEBUG
      cout << "#reads total: " << n_reads_tot << endl;
      cout << "#reads clone " << id_clone << ": "  << map_clone_reads[id_clone] << endl;
      cout << "fraction of genome: " << seq_frac << " (" << seq_len << "*" << copy_number << "/" << genome_len << ")" << endl;
      cout << "clone: " << id_clone << "; CN: " << copy_number << "; cvg: " << cvg << endl;
      cout << "seed: " << rnd_seed << endl;
      cout << "log: " << fn_log.string();
#endif
      art.fold_cvg = cvg;
      //art.num_reads = round(n_reads / 2); // actually specifies read pairs
      art.fn_ref_fa = path_fa.string();
      //art.out_pfx = (path_bam / fn_pfx).string();
      //cout << art.out_pfx << endl;
      // output files are named: <sample>.<clone>.<CN>.sam
      art.rndSeed = rnd_seed;
      art.log = fn_log.string();
      string fn_pfx_out = (path_bam / (lbl_sample+"."+fn_pfx)).string();
      int res_art = art.run(fn_pfx_out);
// TODO: sort and compress SAM output
// TODO: transform BAM tiles by skipping ahead between variant positions
    }
  }
}

bool
BulkSampleGenerator::mergeBulkSeqReads (
  const path path_bam,
  const std::string lbl_sample,
  const vector<BamHeaderRecord>& vec_rg,
  const vario::VariantStore var_store,
  const bool seq_use_vaf,
  RandomNumberGenerator& rng
)
{
  BamFileIn bam_in;
  BamFileOut bam_out;
  BamHeader hdr_out;
  TBamContext bam_context = context(bam_out);

  // initialize read counts for SNV positions
  map<string, unsigned> map_var_cvg;
  map<string, unsigned> map_var_alt;
  for (auto const id_snv : var_store.map_id_snv) {
    map_var_cvg[id_snv.second.id] = 0;
    map_var_alt[id_snv.second.id] = 0;
  }

  // create new output file for sample
  string fn_bam_out = (path_bam / (lbl_sample + ".sam")).string();
  std::ofstream fs_bam_out;
  fs_bam_out.open(fn_bam_out.c_str());

  // collect reference sequence info and write to BAM header
  generateBamHeader(hdr_out, bam_context);
  // attach read groups to BAM header
  addReadGroups(hdr_out, vec_rg);
  // open output file
  if (!open(bam_out, fn_bam_out.c_str(), OPEN_WRONLY | OPEN_CREATE)) {
    fprintf(stderr, "[ERROR] (BulkSampleGenerator::mergeBulkSeqReads)\n");
    fprintf(stderr, "        could not create output file: '%s'\n", fn_bam_out.c_str());
    return false;
  }
  setFormat(bam_out, Sam());
fprintf(stderr, "### BAM output context contains %lu seqs\n", length(contigNames(bam_context)));
  // add RG headers to output BAM
  write(bam_out.iter, hdr_out, bam_context, bam_out.format);

  // loop over tiled (i.e. fragmented) BAM files
  for(auto& entry: directory_iterator(path_bam)) {

    // split file name (expected: <sample>.<clone>.<copy_number>.sam)
    string fn_bam_in = basename(entry);
    vector<string> fn_parts = stringio::split(fn_bam_in, '.');
    if (fn_parts.size() < 3) {
      fprintf(stderr, "[WARN] (BulkSampleGenerator::mergeBulkSeqReads) malformed input file name:\n");
      fprintf(stderr, "       expected: <sample>.<clone>.<cn>.sam\n");
      fprintf(stderr, "       found: %s\n", fn_bam_in.c_str());
      continue;
    }
fprintf(stderr, "### %s\n", fn_bam_in.c_str());
    // does file belong to this sample?
    if (fn_parts[0] == lbl_sample) {
      // open input BAM
      const char* fn_bam_in = entry.path().c_str();
      if (!open(bam_in, fn_bam_in)) {
        fprintf(stderr, "[ERROR] (BulkSampleGenerator::mergeBulkSeqReads) Cannot open BAM file:\n");
        fprintf(stderr, "        %s\n", fn_bam_in);
      }

      const string lbl_clone = fn_parts[1];
      // transform and export alignments to joint BAM 
      // (clone label -> read group id)
      if ( seq_use_vaf ) {
fprintf(stderr, "### BulkSampleGenerator::transformBamTileVaf (%s).\n", fn_bam_in);
        transformBamTileVaf (
          bam_out, 
          bam_in, 
          lbl_clone, 
          var_store, 
          m_samples[lbl_sample].m_map_snv_vaf, 
          rng, 
          map_var_cvg, 
          map_var_alt
        );
      }
      else {
fprintf(stderr, "### BulkSampleGenerator::transformBamTileSeg (%s).\n", fn_bam_in);
        transformBamTileSeg (
          bam_out, 
          bam_in, 
          lbl_clone, 
          var_store, 
          rng, 
          map_var_cvg, 
          map_var_alt
        );
      }

      close(bam_in);

      // delete input SAM tile since it is no longer required (release disk space)
fprintf(stderr, "### Removing input SAM (%s).\n", fn_bam_in);
      remove(entry);
    }
  }

  // output read counts for variable positions within sample
  path fn_read_counts = path_bam / format("%s.vars.csv", lbl_sample.c_str());
  std::ofstream ofs_read_counts(fn_read_counts.string(), std::ofstream::out);
  for (const auto var_cvg : map_var_cvg) {
    ofs_read_counts << var_cvg.first << "\t";
    ofs_read_counts << var_cvg.second << "\t";
    ofs_read_counts << map_var_alt[var_cvg.first] << "\n";
  }
  ofs_read_counts.close();

  return true;
}

void
BulkSampleGenerator::initCloneGenomes (
  const map<string, GenomeInstance> map_lbl_gi,
  const path path_bed
)
{
  fprintf(stdout, "Initializing copy number state for clone genomes...\n");
  
  for (auto const & kv : map_lbl_gi) {
    string lbl_clone = kv.first;
    GenomeInstance genome = kv.second;

    // data structure to keep track of genomic fragments and their CN state
    // ordered by location (chr, start, end);
    map<seqio::TRegion, seqio::AlleleSpecCopyNum> map_reg_cn;

    // infer copy number state segments
    // for each chromosome id, build an interval map
    map<string, interval_map<TCoord, seqio::AlleleSpecCopyNum>> map_chr_alleles;
    genome.getCopyNumberStateByChr(map_chr_alleles);

    // loop over chromosomes
    for ( auto const & chr_seg : map_chr_alleles ) {
      string id_chr = chr_seg.first;
      // loop over segments belonging to chromosome
      for ( auto const & seg : chr_seg.second ) {
        auto itvl = seg.first;
        seqio::AlleleSpecCopyNum cn_state = seg.second;

        TCoord ref_start = itvl.lower();
        TCoord ref_end = itvl.upper();

        // remember CN state for region
        TRegion region = make_tuple(id_chr, ref_start, ref_end);
        map_reg_cn[region] = cn_state;
      }
    }

    // store copy number states in global index
    this->m_clone_reg_cn[lbl_clone] = map_reg_cn;

    
    // store genomic segments for clone by chromosome
    //-------------------------------------------------------------------------

    map<string, seqio::TSegMap> map_chr_seg;
    for (auto const & id_ci : genome.map_id_chr) {
      seqio::TSegMap imap_seg;
      for (auto const & ci : id_ci.second) {
        for (auto const & seg : ci->lst_segments) {
          TCoord c1 = seg.ref_start;
          TCoord c2 = seg.ref_end;
          std::set<SegmentCopy> segset({seg});
          imap_seg += make_pair(interval<TCoord>::right_open(c1,c2), segset);
        }
      }
      map_chr_seg[id_ci.first] = imap_seg;
    }

    this->m_map_clone_chr_seg[lbl_clone] = map_chr_seg;

    // write intervals and corresponding CN state to BED file
    path fn_bed = path_bed / format("%s.cn.bed", lbl_clone.c_str());
    std::ofstream f_bed(fn_bed.string());
    for (auto& reg_cn : map_reg_cn) {
      string id_chr;
      TCoord ref_start, ref_end;
      tie(id_chr, ref_start, ref_end) = reg_cn.first;
      seqio::AlleleSpecCopyNum cn_state = reg_cn.second;
      double cn_total = cn_state.count_A + cn_state.count_B;

      f_bed << format("%s\t%lu\t%lu\t%.2f\t%.2f\n", 
                  id_chr.c_str(), ref_start, ref_end, 
                  cn_state.count_A, cn_state.count_B);
    }
  }
}

// void
// BulkSampleGenerator::writeCloneCnStates (
//   const path path_bed
// )

/** Exporting reference genomes for each genome instance
  * - tiled by copy number state
  * - padded by additional sequence to avoid edge effects
  * - remember total length of each clone genome
  * - remember file names of genome tiles and resp seq len
  */
void
BulkSampleGenerator::writeCloneGenomes (
  const map<string, GenomeInstance> map_lbl_gi,
  const GenomeReference ref_genome,
  unsigned padding,
  unsigned min_len,
  const path path_fasta,
  const path path_bed
)
{
  fprintf(stdout, "Writing tiled ref seqs...\n");
  for (auto const & kv : map_lbl_gi) {
    string label = kv.first;
    GenomeInstance genome = kv.second;
//fprintf(stderr, "\t%s\n", label.c_str());
//cerr << genome;
    // writeFastaTiled(genome, ref_genome, label, padding, min_len, path_fasta, path_bed);
    writeFastaTiled(label, this->m_clone_reg_cn[label], padding, min_len, path_fasta);
  }

  // sanity-check global indices
  assert ( this->m_map_clone_len.size() == map_lbl_gi.size() );
  assert ( this->m_map_fasta_len.size() > 0 );
  this->has_clone_genomes = true;
}

bool
BulkSampleGenerator::calculateBulkCopyNumber (
  //const map<string, map<string, double>> mtx_sample,
  const map<string, GenomeInstance> map_lbl_gi
)
{
  typedef interval_map<TCoord, seqio::AlleleSpecCopyNum> TImapCn;

  // loop over samples
  for (auto & kv : this->m_samples) {
    string id_sample = kv.first;
    map<string, double> w = kv.second.m_clone_weight;

    // merge CN states for individual clone genomes (use interval_map)
    map<string, TImapCn> map_chr_cn;
    for (auto const lbl_w : w) {
      string id_clone = lbl_w.first;
      double weight = lbl_w.second;

      // clones not present in sample do not contribute to copy number.
      if (weight == 0.0) continue;

      GenomeInstance gi = map_lbl_gi.at(id_clone);
      map<string, TImapCn> map_chr_cn_clone;
      gi.getCopyNumberStateByChr(map_chr_cn_clone, weight);

      // for each chromosome id, build an interval map
      for ( auto const & chr_seg : map_chr_cn_clone ) {
        string id_chr = chr_seg.first;
        if (map_chr_cn.count(id_chr) == 0) {
          map_chr_cn[id_chr] = chr_seg.second;
        } else {
          map_chr_cn[id_chr] += chr_seg.second;
        }
      }
    }

    // store genomic intervals and CN state in internal index
    this->m_samples[id_sample].m_chr_cn = map_chr_cn;

    // calculate real genome length for sample (used in exp. cvg. calc)
    TCoord g_len = 0;
    for (auto & chr_cn : map_chr_cn) {
      TImapCn imap_cn = chr_cn.second;
      for (auto it = imap_cn.begin(); it != imap_cn.end(); ++it) {
        TCoord seg_len = it->first.upper() - it->first.lower();
        double seg_cn  = it->second.count_A + it->second.count_B;
        g_len += TCoord(seg_len * seg_cn); 
      }
    }
    this->m_samples[id_sample].genome_len_abs = g_len;
  }

  this->has_cn_states = true;
  
  return true;
}

bool
BulkSampleGenerator::writeBulkCopyNumber (
  const path path_out
) const
{
  // make sure copy number states have been initialized
  assert ( this->has_cn_states );

  typedef interval_map<TCoord, seqio::AlleleSpecCopyNum> TCnMap;

  // loop over samples
  for (auto & id_smp : this->m_samples) {
    string id_sample = id_smp.first;
    map<string, TCnMap> map_chr_cn = id_smp.second.m_chr_cn;

    // create BED file for sample
    path fn_bed = path_out / format("%s.cn.bed", id_sample.c_str());
    std::ofstream ofs_bed(fn_bed.string(), std::ofstream::out);

    // write genomic intervals and CN state to BED file
    for (auto const & chr_seg : map_chr_cn) {
      string id_chr = chr_seg.first;
      interval_map<TCoord, seqio::AlleleSpecCopyNum> imap_reg_cn = chr_seg.second;
      for (auto const & reg_cn : imap_reg_cn) {
        TCoord ref_start, ref_end;
        auto reg = reg_cn.first;
        ref_start = reg.lower();
        ref_end = reg.upper();
        seqio::AlleleSpecCopyNum cn_state = reg_cn.second;
        ofs_bed << format("%s\t%lu\t%lu\t%0.2f\t%0.2f\n", 
          id_chr.c_str(), ref_start, ref_end, cn_state.count_A, cn_state.count_B);
      }
    }
  }

  return true;
}

bool
BulkSampleGenerator::generateBamHeader (
  BamHeader& out_header,
  TBamContext& out_context
)
{
  // require reference sequences to be initialized
  if (!this->has_refseqs) {
    fprintf(stderr, "[ERROR] (BulkSampleGenerator::generateBamHeader)\n");
    fprintf(stderr, "        Requires reference sequences, call initRefSeqs() first.\n");
    return false;
  }

  // for all reference sequences, record name and length
  for (auto kv : m_map_ref_len) {
    string seq_name = kv.first;
    TCoord seq_len = kv.second;
    appendValue(contigNames(out_context), seq_name);
    appendValue(contigLengths(out_context), seq_len);
  }

  return true;
}

void 
BulkSampleGenerator::addReadGroups (
  seqan::BamHeader& header, 
  const vector<seqan::BamHeaderRecord>& vec_rg
) 
{
  for (auto& rg : vec_rg) {
    appendValue(header, rg);
  }
}

void
BulkSampleGenerator::generateReadGroups (
  vector<seqan::BamHeaderRecord>& vec_rg_out, 
  const string id_sample,
  const vector<string>& vec_tag_id,
  const string tag_pl,
  const string tag_pu
)
{
  seqan::BamHeaderRecord record;
  for (auto lbl : vec_tag_id) {
    clear(record);
    record.type = seqan::BAM_HEADER_READ_GROUP;
    appendValue(record.tags, Pair<CharString>());
    assign(back(record.tags).i1, "ID", Exact());
    assign(back(record.tags).i2, lbl, Exact());
    appendValue(record.tags, Pair<CharString>());
    assign(back(record.tags).i1, "SM", Exact());
    assign(back(record.tags).i2, lbl, Exact());
    appendValue(record.tags, Pair<CharString>());
    assign(back(record.tags).i1, "LB", Exact());
    assign(back(record.tags).i2, id_sample, Exact());
    appendValue(record.tags, Pair<CharString>());
    assign(back(record.tags).i1, "PL", Exact());
    assign(back(record.tags).i2, tag_pl, Exact());
    appendValue(record.tags, Pair<CharString>());
    assign(back(record.tags).i1, "PU", Exact());
    assign(back(record.tags).i2, tag_pu, Exact());

    vec_rg_out.push_back(record);
  }
}

bool
BulkSampleGenerator::transformBamTileSeg (
  BamFileOut& bam_out,
  BamFileIn& bam_in,
  const string id_clone,
  const vario::VariantStore& var_store,
  RandomNumberGenerator& rng,
  map<string, unsigned>& map_var_cvg,
  map<string, unsigned>& map_var_alt
)
{
  typedef tuple<TCoord, TCoord, TCoord> TMinMaxOffset;
  map<string, TMinMaxOffset> map_ref_coords; // min, max, offset for ref seq fragments
  map<string, int> map_ref_id_out; // ref seq IDs to use in output BAM alignments
  map<int, int> map_ref_loc_glob; // mapping input to output ref IDs
  BamHeader header_in;
  BamAlignmentRecord read1, read2;
  // used to pick random vector indices (e.g., SegmentCopy)
  random_selector<> selector(rng.generator);
  // used to decide if given read is to be mutated (depending on expected VAF in sample)
  function<double()> r_dbl = rng.getRandomFunctionReal(0.0, 1.0);

  // get SegmentCopy map for clone
  auto it_clone_chr_seg = m_map_clone_chr_seg.find(id_clone);

  // get global ref seq IDs
  auto context_out = context(bam_out);
  for (auto i=0; i<length(contigNames(context_out)); i++) {
    string ref_name(toCString(contigNames(context_out)[i]));
    map_ref_id_out[ref_name] = i;
  }

  // read BAM header
  readHeader(header_in, bam_in);
  auto bam_context = context(bam_in);
#ifndef NDEBUG
  fprintf(stderr, "### BAM input context contains %lu refs.\n", length(contigNames(bam_context)));
#endif
  // map local to global coordinates
  int idx_ref_loc = 0;
  for (auto i=0; i<length(contigNames(bam_context)); i++) {
    string chr = "";
    TCoord loc_start=0, ref_len=0;
    int pad = 0;
     
    CharString seq_name = contigNames(bam_context)[i];
    ref_len = contigLengths(bam_context)[i];
    string ref_id(toCString(seq_name));

    // parse ref seq id, expected format:
    //   <chromosome>_<start>_<end>_<padding>
    vector<string> ref_id_parts = stringio::split(ref_id, '_');
    chr = ref_id_parts[0];
    pad = ref_id_parts.size() > 3 ? stoi(ref_id_parts[3]) : 0;
    if (ref_id_parts.size() >= 3) {
      loc_start = stoul(ref_id_parts[1]);
    } // otherwise, assume complete reference sequence, no padding

    // match local ref ID to global one
    if (map_ref_id_out.count(chr) == 0) {
      fprintf(stderr, "[ERROR] (BulkSampleGenerator::transformBamTile)\n");
      fprintf(stderr, "        unknown ref seq: '%s'\n", chr.c_str());
      return false;
    }
    int ref_id_glob = map_ref_id_out[chr];
    map_ref_loc_glob[idx_ref_loc] = ref_id_glob;
    idx_ref_loc++;

    // determine offset by which to shift local mapping coordinates
    TCoord loc_min = pad;
    TCoord loc_max = ref_len - pad;
    TCoord offset = loc_start - pad;
    TMinMaxOffset coords = make_tuple(loc_min, loc_max, offset);
    map_ref_coords[ref_id] = coords;
  }

  // process read alignments
  unsigned num_reads = 0;
#ifndef NDEBUG
  auto t_start = chrono::steady_clock::now();
  auto t_start_10k = chrono::steady_clock::now();
#endif
  while (!atEnd(bam_in)) {
    readRecord(read1, bam_in);
    readRecord(read2, bam_in);
    num_reads++;

    // get chromosome ID from read mapping
    string r1_ref = toCString(contigNames(bam_context)[read1.rID]);
    string r2_ref = toCString(contigNames(bam_context)[read2.rID]);
    // determine local->global coordinate mapping
    TCoord min_loc = 0, max_loc = 0, off_glob = 0;
    tie(min_loc, max_loc, off_glob) = map_ref_coords[r1_ref];

    // update alignment ref seq
    int rid_new = map_ref_loc_glob[read1.rID];
    read1.rID = rid_new;
    read2.rID = rid_new;
    read1.rNextId = rid_new;
    read2.rNextId = rid_new;

    // update alignment coordinates
    read1.beginPos += off_glob;
    read2.beginPos += off_glob;

    // parse read alignment details
    int r1_begin = read1.beginPos;
    int r2_begin = read2.beginPos;
    int r1_len = getAlignmentLengthInRef(read1);
    int r2_len = getAlignmentLengthInRef(read2);
    int r1_end = r1_begin + r1_len;
    int r2_end = r2_begin + r2_len;
    char r1_rc = hasFlagRC(read1) ? '+' : '-';
    char r2_rc = hasFlagRC(read2) ? '+' : '-';

    // check if mapping lies within target coordinates
    if (r1_begin < min_loc || r2_begin < min_loc || r1_end > max_loc || r2_end > max_loc) {
      fprintf(stderr, "[INFO] (BulkSampleGenerator::transformBamTile) discarding read pair because of coordinate constraints:\n");
      fprintf(stderr, "       %s (%d..%d), %s (%d..%d)\n", toCString(read1.qName), r1_begin, r1_end, toCString(read2.qName), r2_begin, r2_end);
      continue;
    }

    // assign read group
    CharString tagRG = format("RG:Z:%s", toCString(id_clone));
    appendTagsSamToBam(read1.tags, tagRG);
    appendTagsSamToBam(read2.tags, tagRG);

    // spike in mutations
    string chr(toCString(contigNames(context_out)[rid_new]));
    map<TCoord, vector<int>> map_pos_var = var_store.map_chr_pos_snvs.at(chr);
    seqio::TSegMap segments = it_clone_chr_seg->second[chr];

    //--- MUTATE READ PAIR (BEGIN) ---
    // Code copied from mutateReadPairSeg() for increased runtime performance. 

    SegmentCopy seg(0);
  
    // determine read pair coordinates
    TCoord pos_begin, pos_end;
    if (r1_begin < r2_begin) {
      pos_begin = r1_begin;
      pos_end   = r2_end;
    }
    else {
      pos_begin = r2_begin;
      pos_end   = r1_end;
    }
  
    // increase read count for variants overlapping read pair.
    for (auto it = map_pos_var.lower_bound(pos_begin); 
         it != map_pos_var.end() && it->first <= pos_end;
         ++it)
    {
      if ( it->first >= r1_begin && it->first < r1_end ) { // read1 overlaps with variant
        for ( int v : it->second ) {
          map_var_cvg[var_store.map_id_snv.at(v).id]++;
        }
      }
      else if ( it->first >= r2_begin && it->first < r2_end ) { // read1 overlaps with variant
        for ( int v : it->second ) {
          map_var_cvg[var_store.map_id_snv.at(v).id]++;
        }
      }
    }

    // 1. Assign SegmentCopy for read pair.
    //--------------------------------------
  
    // determine SegmentCopies overlapping with read pair mapping coords
    seqio::TSegSet iset_read;
    iset_read.add(interval<TCoord>::right_open(pos_begin, pos_end));
    seqio::TSegMap imap_seg_avail = segments & iset_read;

    if (imap_seg_avail.size()>0) {
      // flatten interval map
      vector<SegmentCopy> vec_seg;
      for (auto const & itvl_iset : imap_seg_avail) {
        for (const SegmentCopy & segment : itvl_iset.second)
          vec_seg.push_back(segment);
      }
      seg = selector(vec_seg);

/* fprintf(stderr, "#%s#\t%s\t%s\t%lu\t%lu\t%d\t%d\t%d\t%d\n", 
  id_clone.c_str(), 
  boost::uuids::to_string(seg.id).c_str(),
  toCString(read1.qName), 
  pos_begin,
  pos_end,
  r1_begin, r1_end,
  r2_begin, r2_end); */

    } 
    else {
      fprintf(stderr, "[WARN] (BulkSampleGenerator::transformBamTile)\n");
      fprintf(stderr, "       No genomic segment copy found for read pair '%s'\n", toCString(read1.qName));
    }
  
    // 2. Get variants associated with SegmentCopy.
    //----------------------------------------------
  
    map<seqio::TCoord, vector<Variant>> map_pos_mut;
    //var_store.getSnvsForSegmentCopy(map_pos_mut, seg.id);
    var_store.getSnvsForSegmentCopy(map_pos_mut, seg.id, pos_begin, pos_end);
  
    // 3. Apply variants overlapping read pair.
    //------------------------------------------
  
    for (auto it = map_pos_mut.lower_bound(pos_begin); 
         it != map_pos_mut.end() && it->first <= pos_end;
         ++it)
    {
      for (Variant var : it->second) {
  
        int r1_var_pos = var.pos - r1_begin;
        if (r1_var_pos >= 0 && r1_var_pos < r1_len) { // read1 overlaps with variant
          map_var_alt[var.id]++;
          read1.seq[r1_var_pos] = var.alleles[1][0];
        }
  
        int r2_var_pos = var.pos - r2_begin;
        if (r2_var_pos >= 0 && r2_var_pos < r2_len) { // read2 overlaps with variant
          map_var_alt[var.id]++;
          read2.seq[r2_var_pos] = var.alleles[1][0];
        }
      }
    }

    //--- MUTATE READ PAIR (END) ---

#ifndef NDEBUG
    if (num_reads % 10000 == 0) {
      auto t_end_10k = chrono::steady_clock::now();
      auto t_diff = t_end_10k - t_start_10k;
      fprintf(stderr, "### Mutated %07d read pairs.\n", num_reads);
      fprintf(stderr, "### TIME: %.2f ms for 10000 read pairs.\n", chrono::duration <double, milli> (t_diff).count());
      t_start_10k = chrono::steady_clock::now();
    }
#endif
    
    // mutateReadPair(read1, read2, it_clone_chr_seg->second[chr], var_store, selector);
    // mutateReadPairVaf(read1, read2,
    //                   var_store.map_chr_pos_snvs.at(chr),
    //                   var_store.map_id_snv,
    //                   r_dbl,
    //                   r1_begin,
    //                   r2_begin,
    //                   r1_end,
    //                   r2_end
    // );

    // write updated reads to output BAM
    write(bam_out.iter, read1, context(bam_out), bam_out.format);
    write(bam_out.iter, read2, context(bam_out), bam_out.format);
  }

#ifndef NDEBUG
  auto t_end = chrono::steady_clock::now();
  auto t_diff = t_end - t_start;
  fprintf(stderr, "### Processed %d read pairs.\n", num_reads);
  fprintf(stderr, "### TIME: %.2f ms for %d read pairs.\n", chrono::duration <double, milli> (t_diff).count(), num_reads);
#endif

  return true;
}

bool
BulkSampleGenerator::transformBamTileVaf (
  BamFileOut& bam_out,
  BamFileIn& bam_in,
  const string id_clone,
  const vario::VariantStore& var_store,
  const map<int, double>& map_snv_vaf,
  RandomNumberGenerator& rng,
  map<string, unsigned>& map_var_cvg,
  map<string, unsigned>& map_var_alt
)
{
  typedef tuple<TCoord, TCoord, TCoord> TMinMaxOffset;
  map<string, TMinMaxOffset> map_ref_coords; // min, max, offset for ref seq fragments
  map<string, int> map_ref_id_out; // ref seq IDs to use in output BAM alignments
  map<int, int> map_ref_loc_glob; // mapping input to output ref IDs
  BamHeader header_in;
  BamAlignmentRecord read1, read2;
  // used to pick random vector indices (e.g., SegmentCopy)
  random_selector<> selector(rng.generator);
  // used to decide if given read is to be mutated (depending on expected VAF in sample)
  function<double()> r_dbl = rng.getRandomFunctionReal(0.0, 1.0);

  // get SegmentCopy map for clone
  auto it_clone_chr_seg = m_map_clone_chr_seg.find(id_clone);

  // get global ref seq IDs
  auto context_out = context(bam_out);
  for (auto i=0; i<length(contigNames(context_out)); i++) {
    string ref_name(toCString(contigNames(context_out)[i]));
    map_ref_id_out[ref_name] = i;
  }

  // read BAM header
  readHeader(header_in, bam_in);
  auto bam_context = context(bam_in);
fprintf(stderr, "### BAM input context contains %lu refs.\n", length(contigNames(bam_context)));
  // map local to global coordinates
  int idx_ref_loc = 0;
  for (auto i=0; i<length(contigNames(bam_context)); i++) {
    string chr = "";
    TCoord loc_start=0, ref_len=0;
    int pad = 0;
     
    CharString seq_name = contigNames(bam_context)[i];
    ref_len = contigLengths(bam_context)[i];
    string ref_id(toCString(seq_name));

    // parse ref seq id, expected format:
    //   <chromosome>_<start>_<end>_<padding>
    vector<string> ref_id_parts = stringio::split(ref_id, '_');
    chr = ref_id_parts[0];
    pad = ref_id_parts.size() > 3 ? stoi(ref_id_parts[3]) : 0;
    if (ref_id_parts.size() >= 3) {
      loc_start = stoul(ref_id_parts[1]);
    } // otherwise, assume complete reference sequence, no padding

    // match local ref ID to global one
    if (map_ref_id_out.count(chr) == 0) {
      fprintf(stderr, "[ERROR] (BulkSampleGenerator::transformBamTile)\n");
      fprintf(stderr, "        unknown ref seq: '%s'\n", chr.c_str());
      return false;
    }
    int ref_id_glob = map_ref_id_out[chr];
    map_ref_loc_glob[idx_ref_loc] = ref_id_glob;
    idx_ref_loc++;

    // determine offset by which to shift local mapping coordinates
    TCoord loc_min = pad;
    TCoord loc_max = ref_len - pad;
    TCoord offset = loc_start - pad;
    TMinMaxOffset coords = make_tuple(loc_min, loc_max, offset);
    map_ref_coords[ref_id] = coords;
  }

  // process read alignments
  unsigned num_reads = 0;
auto t_start = chrono::steady_clock::now();
auto t_start_10k = chrono::steady_clock::now();

  while (!atEnd(bam_in)) {
    readRecord(read1, bam_in);
    readRecord(read2, bam_in);
    num_reads++;

    // parse read alignment details
    int r1_begin = read1.beginPos;
    int r2_begin = read2.beginPos;
    int r1_len = getAlignmentLengthInRef(read1);
    int r2_len = getAlignmentLengthInRef(read2);
    int r1_end = r1_begin + r1_len;
    int r2_end = r2_begin + r2_len;
    string r1_ref = toCString(contigNames(bam_context)[read1.rID]);
    string r2_ref = toCString(contigNames(bam_context)[read2.rID]);
    char r1_rc = hasFlagRC(read1) ? '+' : '-';
    char r2_rc = hasFlagRC(read2) ? '+' : '-';

    // determine local->global coordinate mapping
    TCoord min_loc = 0, max_loc = 0, off_glob = 0;
    tie(min_loc, max_loc, off_glob) = map_ref_coords[r1_ref];

    // check if mapping lies within target coordinates
    if (r1_begin < min_loc || r2_begin < min_loc || r1_end > max_loc || r2_end > max_loc) {
      fprintf(stderr, "[INFO] (BulkSampleGenerator::transformBamTile) discarding read pair because of coordinate constraints:\n");
      fprintf(stderr, "       %s (%d..%d), %s (%d..%d)\n", toCString(read1.qName), r1_begin, r1_end, toCString(read2.qName), r2_begin, r2_end);
      continue;
    }

    // update alignment ref seq
    int rid_new = map_ref_loc_glob[read1.rID];
    read1.rID = rid_new;
    read2.rID = rid_new;
    read1.rNextId = rid_new;
    read2.rNextId = rid_new;

    // update alignment coordinates
    read1.beginPos += off_glob;
    read2.beginPos += off_glob;
    r1_begin += off_glob;
    r2_begin += off_glob;
    r1_end += off_glob;
    r2_end += off_glob;

    // assign read group
    CharString tagRG = format("RG:Z:%s", toCString(id_clone));
    appendTagsSamToBam(read1.tags, tagRG);
    appendTagsSamToBam(read2.tags, tagRG);

    // spike in mutations
    string chr(toCString(contigNames(context_out)[rid_new]));

#ifndef NDEBUG 
    if (num_reads % 10000 == 0) {
      auto t_end_10k = chrono::steady_clock::now();
      auto t_diff = t_end_10k - t_start_10k;
      fprintf(stderr, "### Mutated %07d read pairs.\n", num_reads);
      fprintf(stderr, "### TIME: %.2f ms for 10000 read pairs.\n", chrono::duration <double, milli> (t_diff).count());
      t_start_10k = chrono::steady_clock::now();
    }
#endif

    //auto map_pos_snv = var_store.map_chr_pos_snvs.at(chr); 
    //auto map_id_snv = var_store.map_id_snv;
    
    // determine read pair coordinates
    TCoord pos_begin, pos_end;
    if (r1_begin < r2_begin) {
      pos_begin = r1_begin;
      pos_end   = r2_end;
    }
    else {
      pos_begin = r2_begin;
      pos_end   = r1_end;
    }

    // identify first variant larger than read pair start
    auto it_snv = var_store.map_chr_pos_snvs.at(chr).lower_bound(pos_begin);
    
    // loop over candidate variants
    while ( it_snv != var_store.map_chr_pos_snvs.at(chr).end() && it_snv->first < pos_end ) {
      seqio::TCoord pos_var = it_snv->first;
      // each position can be affected by multiple SNVs (relaxing infinite sites assumption)
      vector<int> vec_snv = it_snv->second;
      for (int id_snv : vec_snv) {
        // get variant allele frequency for SNV in this sample
        double vaf = map_snv_vaf.at(id_snv);

        Variant snv = var_store.map_id_snv.at(id_snv);

        if (pos_var >= r1_begin && pos_var < r1_end) { // variant overlaps read1
          map_var_cvg[snv.id]++;

          // decide if read pair is to be mutated (with probability VAF)
          if (r_dbl() <= vaf) {
            int r1_var_pos = pos_var - r1_begin;
            read1.seq[r1_var_pos] = snv.alleles[1][0];
            map_var_alt[snv.id]++;
          }
        } 
        else if (pos_var >= r2_begin && pos_var < r2_end) { // variant overlaps read2
          map_var_cvg[snv.id]++;

          // decide if read pair is to be mutated (with probability VAF)
          if (r_dbl() <= vaf) {
            int r2_var_pos = pos_var - r2_begin;
            read2.seq[r2_var_pos] = snv.alleles[1][0];
            map_var_alt[snv.id]++;
          }
        }
      }
      // next variable position
      it_snv++;
    }

    // write updated reads to output BAM
    write(bam_out.iter, read1, context(bam_out), bam_out.format);
    write(bam_out.iter, read2, context(bam_out), bam_out.format);
  }

auto t_end = chrono::steady_clock::now();
auto t_diff = t_end - t_start;
fprintf(stderr, "### Processed %d read pairs.\n", num_reads);
fprintf(stderr, "### TIME: %.2f ms for %d read pairs.\n", chrono::duration <double, milli> (t_diff).count(), num_reads);

  return true;
}

bool BulkSampleGenerator::mutateReadPairSeg (
  BamAlignmentRecord& read1, 
  BamAlignmentRecord& read2,
  const seqio::TSegMap& segments,
  const vario::VariantStore& var_store,
  random_selector<>& selector
)
{
  SegmentCopy seg;
  int r1_begin = read1.beginPos;
  int r2_begin = read2.beginPos;
  int r1_len = getAlignmentLengthInRef(read1);
  int r2_len = getAlignmentLengthInRef(read2);
  int r1_end = r1_begin + r1_len;
  int r2_end = r2_begin + r2_len;

  // determine read pair coordinates
  TCoord pos_begin, pos_end;
  if (r1_begin < r2_begin) {
    pos_begin = r1_begin;
    pos_end   = r2_end;
  }
  else {
    pos_begin = r2_begin;
    pos_end   = r1_end;
  }

  // 1. Assign SegmentCopy for read pair.
  //--------------------------------------

  // determine SegmentCopies overlapping with read pair mapping coords
  seqio::TSegSet iset_read;
  iset_read.add(interval<TCoord>::right_open(pos_begin, pos_end));
  seqio::TSegMap imap_seg_avail = segments & iset_read;
  //SegmentCopy seg = selector(imap_seg_avail);
  if (imap_seg_avail.size()>0) {
    // flatten interval map
    vector<SegmentCopy> vec_seg;
    for (auto const & itvl_iset : imap_seg_avail) {
      for (const SegmentCopy & segment : itvl_iset.second)
        vec_seg.push_back(segment);
    }
    seg = selector(vec_seg);
  } 
  else {
    fprintf(stderr, "[WARN] (BulkSampleGenerator::transformBamTile)\n");
    fprintf(stderr, "       No genomic segment copy found for read pair '%s'\n", toCString(read1.qName));
  }

  // 2. Get variants associated with SegmentCopy.
  //----------------------------------------------

  map<seqio::TCoord, vector<Variant>> map_pos_var;
  //var_store.getSnvsForSegmentCopy(map_pos_var, seg.id);
  var_store.getSnvsForSegmentCopy(map_pos_var, seg.id, r1_begin, r2_end);

  // 3. Apply variants overlapping read pair.
  //------------------------------------------

  for (auto it = map_pos_var.lower_bound(pos_begin); 
       it != map_pos_var.end() && it->first <= pos_end;
       ++it)
  {
    for (Variant var : it->second) {

      int r1_var_pos = var.pos - r1_begin;
      if (r1_var_pos >= 0 && r1_var_pos < r1_len) { // read1 overlaps with variant
        //map_var_cvg[var.id]++;
        //map_var_vaf[var.id]++;
        //fs_log << str(boost::format("%s:%d\t%s->%s\n") % toCString(read1.qName) % r1_var_pos % var.alleles[0].c_str() % var.alleles[1].c_str());
        read1.seq[r1_var_pos] = var.alleles[1][0];
      }

      int r2_var_pos = var.pos - r2_begin;
      if (r2_var_pos >= 0 && r2_var_pos < r2_len) { // read2 overlaps with variant
        //map_var_cvg[var.id]++;
        //map_var_vaf[var.id]++;
        //fs_log << str(boost::format("%s:%d\t%s->%s\n") % toCString(read2.qName) % r2_var_pos % var.alleles[0].c_str() % var.alleles[1].c_str());
        read2.seq[r2_var_pos] = var.alleles[1][0];
      }
    }
  }

  return true;
}

bool BulkSampleGenerator::mutateReadPairVaf (
  BamAlignmentRecord& read1, 
  BamAlignmentRecord& read2,
  const map<seqio::TCoord, vector<int>>& map_pos_snv,
  const map<int, vario::Variant>& map_id_snv,
  const map<int, double>& map_snv_vaf,
  function<double()>& r_dbl,
  const int r1_begin,
  const int r2_begin,
  const int r1_end,
  const int r2_end
)
{
  // determine read pair coordinates
  TCoord pos_begin, pos_end;
  if (r1_begin < r2_begin) {
    pos_begin = r1_begin;
    pos_end   = r2_end;
  }
  else {
    pos_begin = r2_begin;
    pos_end   = r1_end;
  }

  // identify first variant larger than read pair start
  auto it_snv = map_pos_snv.lower_bound(pos_begin);
  
  // loop over candidate variants
  while ( it_snv != map_pos_snv.end() && it_snv->first < pos_end ) {
    seqio::TCoord pos_var = it_snv->first;
    // each position can be affected by multiple SNVs (relaxing infinite sites assumption)
    vector<int> vec_snv = it_snv->second;
    for (int id_snv : vec_snv) {
      // get variant allele frequency for SNV in this sample
      double vaf = map_snv_vaf.at(id_snv);

      // decide if read pair is to be mutated (with probability VAF)
      if (r_dbl() > vaf) continue;

      Variant snv = map_id_snv.at(id_snv);

      if (pos_var >= r1_begin && pos_var < r1_end) { // mutate read1
        int r1_var_pos = pos_var - r1_begin;
        read1.seq[r1_var_pos] = snv.alleles[1][0];
      } 
      else if (pos_var >= r2_begin && pos_var < r2_end) { // mutate read2
        int r2_var_pos = pos_var - r2_begin;
        read2.seq[r2_var_pos] = snv.alleles[1][0];
      }
    }
    // next variable position
    it_snv++;
  }
  
  return true;
}

int
BulkSampleGenerator::writeExpectedReadCounts (
  std::ofstream& ofs_out,
  const int cvg_depth,
  const vario::VariantStore& var_store,
  const map<int, double>& map_snv_vaf
) const
{
  // write header
  ofs_out << "# Expected read counts for bulk sample." << endl;
  ofs_out << "# id_snv,chr,pos,ref,alt" << endl;

  // loop over variants
  for (auto const & snv_vaf : map_snv_vaf) {
    int id_snv = snv_vaf.first;
    double vaf = snv_vaf.second;

    // get SNV details
    Variant var = var_store.map_id_snv.at(id_snv);
    
    // write output to file
    ofs_out << id_snv << ",";
    ofs_out << var.chr << ",";
    ofs_out << var.pos << ",";
    ofs_out << vaf << endl;
  }
}

/** NOTE: Requires that initCloneGenomes() has been called before! */
void
BulkSampleGenerator::writeFastaTiled (
  const string lbl_clone,
  const map<seqio::TRegion, seqio::AlleleSpecCopyNum> map_reg_cn,
  const TCoord padding,
  const TCoord min_len,
  const path path_fasta) 
{
  int line_width = 60; // TODO: should this be a parameter?
  string str_pad(padding, 'A');

  // keep track of sequence lengths in genomic tiles
  map<int, unsigned long long> map_cn_len;
  // keep track of number of sequences per CN state
  map<int, unsigned> map_cn_nseq;

  // export genomic fragments to corresponding output files
  map<int, shared_ptr<std::ofstream>> map_cn_file;
  
  //for ( auto const & chr_seg : map_chr_alleles ) {
  for ( auto const & reg_cn : map_reg_cn ) {
    string chr;
    TCoord start, end;
    tie(chr, start, end) = reg_cn.first;
    seqio::AlleleSpecCopyNum cn = reg_cn.second;

    // ignore segments shorter than minimum length
    if ( end-start < min_len ) 
      continue;

    int cn_total = cn.count_A + cn.count_B;

    // create output file for CN state if not exists
    if ( map_cn_file.count(cn_total) == 0 ) {
      path filepath = path_fasta / format("%s.%d.fa", lbl_clone.c_str(), cn_total);
#ifndef NDEBUG
      fprintf(stderr, "### [BulkSampleGenerator::writeFastaTiled] Creating file '%s'\n", filepath.string().c_str());
#endif
      shared_ptr<std::ofstream> ofs(new std::ofstream(filepath.string(), std::ofstream::out));
      map_cn_file[cn_total] = ofs;
    }
    shared_ptr<std::ofstream> ofs = map_cn_file[cn_total];

    // get sequence for target region from reference genome
    map<TCoord, string> map_start_seq;
    this->m_ref_genome->getSequence(chr, start, end, map_start_seq);
    vector<shared_ptr<SeqRecord>> sequences;
    for (auto const & start_seq : map_start_seq) {
      TCoord start = start_seq.first;
      string seq = start_seq.second;
      TCoord end = start + seq.length();
      string id_rec = format("%s_%lu_%lu_%u", chr.c_str(), start, end, padding);
      shared_ptr<SeqRecord> rec(new SeqRecord(id_rec, "", str_pad+seq+str_pad));
      sequences.push_back(rec);

      // add fragment length to CN->len index
      if (map_cn_len.count(cn_total) == 0) {
        map_cn_len[cn_total] = (end-start) + 2*padding;
        map_cn_nseq[cn_total] = 1;
      } else {
        map_cn_len[cn_total] += (end-start) + 2*padding;
        map_cn_nseq[cn_total] += 1;
      }
    }
    
    writeFasta(sequences, *ofs, line_width);
  }

  // close output file streams
  for (auto cn_file : map_cn_file) {
    assert( cn_file.second->is_open() );
    cn_file.second->close();
  }

  // update global indices
  // (used to calculate sequencing coverage for segments)
  //---------------------------------------------------------------------------

  // 1.a) no. sequences by CN
  // 1.b) seq length by CN 
  // 1.c) total seq length by clone
  unsigned long long genome_len = 0;
  for (auto const & cn_len : map_cn_len) {
    int cn_state = cn_len.first;
    unsigned long long seq_len = cn_len.second;
    unsigned num_seqs = map_cn_nseq[cn_state];

    // update FASTA file indices
    path filepath = path_fasta / format("%s.%d.fa", lbl_clone.c_str(), cn_state);
    this->m_map_fasta_len[filepath] = seq_len;
    this->m_map_fasta_nseq[filepath] = num_seqs;

    // add sequence copies to total genome length
    genome_len += cn_state * seq_len;
  }
  this->m_map_clone_len[lbl_clone] = genome_len;
}

void
BulkSampleGenerator::writeFastaTiledBak (
  const seqio::GenomeInstance genome,
  const seqio::GenomeReference reference,
  const std::string lbl_clone,
  unsigned int padding,
  unsigned int min_len,
  const path path_fasta,
  const path path_bed) 
{
  int line_width = 60; // TODO: should this be a parameter?
  string str_pad(padding, 'A');

  // infer copy number state segments
  // for each chromosome id, build an interval map
  map<string, interval_map<TCoord, seqio::AlleleSpecCopyNum>> map_chr_alleles;
  genome.getCopyNumberStateByChr(map_chr_alleles);

  // data structure to keep track of genomic fragments and their CN state
  // ordered by location (chr, start, end);
  map<seqio::TRegion, seqio::AlleleSpecCopyNum> map_reg_cn;

  // export genomic fragments to corresponding output files
  map<int, shared_ptr<std::ofstream>> map_cn_file;
  // for ( auto const & chr_seg : map_chr_segments ) {
  for ( auto const & chr_seg : map_chr_alleles ) {
    string id_chr = chr_seg.first;
    for ( auto const & seg : chr_seg.second ) {
      auto i = seg.first;
      TCoord ref_start = i.lower();
      TCoord ref_end = i.upper();
      // ignore segments shorter than minimum length
      if ( ref_end-ref_start < min_len ) 
        continue;
      // double cn_state = seg.second;
      seqio::AlleleSpecCopyNum cn_state = seg.second;
      double cn_total = cn_state.count_A + cn_state.count_B;
      // create output file for CN state if not exists
      if ( map_cn_file.count(cn_total) == 0 ) {
        path filepath = path_fasta / format("%s.%d.fa", lbl_clone.c_str(), cn_total);
        shared_ptr<std::ofstream> ofs(new std::ofstream(filepath.string(), std::ofstream::out));
        map_cn_file[cn_total] = ofs;
      }
      shared_ptr<std::ofstream> ofs = map_cn_file[cn_total];
      // get sequence for target region from reference genome
      map<TCoord, string> map_start_seq;
      reference.getSequence(id_chr, ref_start, ref_end, map_start_seq);
      vector<shared_ptr<SeqRecord>> sequences;
      for (auto const & start_seq : map_start_seq) {
        TCoord start = start_seq.first;
        string seq = start_seq.second;
        TCoord end = start + seq.length();
        string id_rec = format("%s_%lu_%lu_%u", id_chr.c_str(), start, end, padding);
        shared_ptr<SeqRecord> rec(new SeqRecord(id_rec, "", str_pad+seq+str_pad));
        sequences.push_back(rec);

        // remember CN state for region
        TRegion region = make_tuple(id_chr, start, end);
        map_reg_cn[region] = cn_state;
      }
      //num_records = writeFasta(sequences, *ofs, line_width);
      writeFasta(sequences, *ofs, line_width);
    }
  }
  // close output file streams
  for (auto cn_file : map_cn_file) {
    assert( cn_file.second->is_open() );
    cn_file.second->close();
    //*ofs << str(boost::format("%s\t%lu\t%lu\n") % id_chr % ref_start % ref_end);
  }

  // keep track of sequence lengths in genomic tiles
  map<int, unsigned long long> map_cn_len;
  // keep track of number of sequences per CN state
  map<int, unsigned> map_cn_nseq;

  // write intervals and corresponding CN state to BED file
  path fn_bed = path_bed / format("%s.cn.bed", lbl_clone.c_str());
  std::ofstream f_bed(fn_bed.string());
  for (auto& reg_cn : map_reg_cn) {
    string id_chr;
    TCoord ref_start, ref_end;
    tie(id_chr, ref_start, ref_end) = reg_cn.first;
    seqio::AlleleSpecCopyNum cn_state = reg_cn.second;
    double cn_total = cn_state.count_A + cn_state.count_B;

    f_bed << format("%s\t%lu\t%lu\t%.2f\t%.2f\n", 
                    id_chr.c_str(), ref_start, ref_end, 
                    cn_state.count_A, cn_state.count_B);

    // add fragment length to CN->len index
    if (map_cn_len.count(cn_total) == 0) {
      map_cn_len[cn_total] = (ref_end - ref_start) + 2*padding;
      map_cn_nseq[cn_total] = 1;
    } else {
      map_cn_len[cn_total] += (ref_end - ref_start) + 2*padding;
      map_cn_nseq[cn_total] += 1;
    }
  }

  // update global indices
  //-----------------------

  // 1.a) no. sequences by CN
  // 1.b) seq length by CN 
  // 1.c) total seq length by clone
  unsigned long long genome_len = 0;
  for (auto const & cn_len : map_cn_len) {
    int cn_state = cn_len.first;
    unsigned long long seq_len = cn_len.second;
    unsigned num_seqs = map_cn_nseq[cn_state];

    // update FASTA file indices
    path filepath = path_fasta / format("%s.%d.fa", lbl_clone.c_str(), cn_state);
    this->m_map_fasta_len[filepath] = seq_len;
    this->m_map_fasta_nseq[filepath] = num_seqs;

    // add sequence copies to total genome length
    genome_len += cn_state * seq_len;
  }
  this->m_map_clone_len[lbl_clone] = genome_len;

  // 2. genomic segments by clone and chromosome
  map<string, seqio::TSegMap> map_chr_seg;
  for (auto const & id_ci : genome.map_id_chr) {
    seqio::TSegMap imap_seg;
    for (auto const & ci : id_ci.second) {
      for (auto const & seg : ci->lst_segments) {
        TCoord c1 = seg.ref_start;
        TCoord c2 = seg.ref_end;
        std::set<SegmentCopy> segset({seg});
        imap_seg += make_pair(interval<TCoord>::right_open(c1,c2), segset);
      }
    }
    map_chr_seg[id_ci.first] = imap_seg;
  }
// DEBUG output
cerr << "genomic segment index for clone " << lbl_clone << endl;
for (auto const & kv : map_chr_seg) {
  cerr << "  " << kv.first << endl;
  for (auto const & iseg : kv.second) {
    fprintf(stderr, "    [%lu,%lu)\n", iseg.first.lower(), iseg.first.upper());
    for (auto const & s : iseg.second) {
      fprintf(stderr, "    %s [%lu,%lu)\n", boost::uuids::to_string(s.id).c_str(), s.ref_start, s.ref_end);
    }
  }
}
  this->m_map_clone_chr_seg[lbl_clone] = map_chr_seg;
}

} /* namespace bamio */
