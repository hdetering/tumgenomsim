#include <boost/test/unit_test.hpp>
#include <boost/timer/timer.hpp>

#include "../core/random.hpp"
#include "../core/seqio.hpp"
#include "../core/vario.hpp"
#include <boost/format.hpp>
#include <fstream>
#include <vector>
using boost::format;
using evolution::SubstitutionModel;
using namespace std;
using namespace vario;

struct FixtureVario {
  FixtureVario() {
    BOOST_TEST_MESSAGE( "setup fixure" );
    fn_vcf = "data/germline.vcf";

    unsigned long ref_genome_len = 1000000;
    vector<double> nuc_freqs = { 0.3, 0.2, 0.2, 0.3 };
    // initialize random number generator

    BOOST_TEST_MESSAGE( "generating random genome (" << ref_genome_len << " bp)... ");
    ref_genome.generate(ref_genome_len, nuc_freqs, rng);

    BOOST_TEST_MESSAGE( "indexing genome... ");
    ref_genome.indexRecords();

    double Q[4][4] = {
      {    0.0, 0.0406, 0.1661, 0.0331 },
      { 0.0417,    0.0, 0.0438, 0.1746 },
      { 0.1744, 0.0440,    0.0, 0.0418 },
      { 0.0331, 0.1662, 0.0405,    0.0 }
    };
    model = SubstitutionModel(Q);
  }
  ~FixtureVario() {
    BOOST_TEST_MESSAGE( "teardown fixture" );
  }

  string fn_vcf;
  SubstitutionModel model;
  seqio::Genome ref_genome;
  long seed = 123456789;
  RandomNumberGenerator<> rng = RandomNumberGenerator<>(seed);
};

BOOST_FIXTURE_TEST_SUITE( vario, FixtureVario )

/* test some random functions */
BOOST_AUTO_TEST_CASE( random )
{
  BOOST_TEST_MESSAGE( " Picking indices based on weights (0.1, 0.2, 0,3, 0.4)... " );

  vector<int> counts(4, 0);
  vector<double> probs = { 0.1, 0.2, 0.3, 0.4 };
  boost::function<int()> rand_idx = rng.getRandomIndexWeighted(probs);

  for (int i=0; i<100000; ++i) {
    counts[rand_idx()]++;
  }

  for (int i=0; i<4; ++i) {
    BOOST_TEST_MESSAGE( format("%d: %d") % i % counts[i] );
  }

  BOOST_TEST_MESSAGE( " Starting timer ... NOW! " );
  boost::timer::auto_cpu_timer t;
  BOOST_TEST_MESSAGE( " Generating random gamma-distributed numbers (shape=2.0, scale=0.5)... " );
  auto fn_out = "gamma.k5_s1.csv";
  ofstream fs_out;
  fs_out.open(fn_out);

  auto random_gamma = rng.getRandomGamma(5.0, 1.0);
  for (int i=0; i<100000; ++i) {
    fs_out << random_gamma() << endl;
  }
  fs_out.close();
  BOOST_TEST_MESSAGE( " Output numbers are in 'gamma.k5_s1.csv' " );
}

/*
BOOST_AUTO_TEST_CASE( vcf_sanity_check )
{

}
*/

/* read benchmark variant set and calculate sumstats */
BOOST_AUTO_TEST_CASE( vcf_sumstats )
{
  VariantSet variant_set;
  vector<vector<Genotype> > gtMatrix;
  BOOST_TEST_MESSAGE( "loading benchmark VCF file '" << fn_vcf << "'..." );
  readVcf(fn_vcf, variant_set, gtMatrix);
  vector<Variant> variants = variant_set.vec_variants;
  BOOST_TEST_MESSAGE( "  read " << variants.size() << " variants." );
  BOOST_CHECK( variants.size() == 3100839 );
  BOOST_CHECK( gtMatrix.size() == 1 );
  BOOST_CHECK( gtMatrix[0].size() == variants.size() );

  BOOST_TEST_MESSAGE( "calculating VCF sumstats..." );
  //variant_set.calculateSumstats();
  double (&f)[4][4] = variant_set.mat_freqs;

  BOOST_TEST_MESSAGE( "\tnucleotide substitution frequencies:" );
  BOOST_TEST_MESSAGE( "   |    A   |    C   |    G   |    T   " );
  BOOST_TEST_MESSAGE( format(" A | %0.4f | %0.4f | %0.4f | %0.4f ") % f[0][0] % f[0][1] % f[0][2] % f[0][3] );
  BOOST_TEST_MESSAGE( format(" C | %0.4f | %0.4f | %0.4f | %0.4f ") % f[1][0] % f[1][1] % f[1][2] % f[1][3] );
  BOOST_TEST_MESSAGE( format(" G | %0.4f | %0.4f | %0.4f | %0.4f ") % f[2][0] % f[2][1] % f[2][2] % f[2][3] );
  BOOST_TEST_MESSAGE( format(" T | %0.4f | %0.4f | %0.4f | %0.4f ") % f[3][0] % f[3][1] % f[3][2] % f[3][3] );
}

/* generate set of novel variants */
BOOST_AUTO_TEST_CASE( generate_variants )
{
  boost::timer::auto_cpu_timer t;
  BOOST_TEST_MESSAGE( "duplicating reference chromosomes..." );
  ref_genome.duplicate();
  BOOST_TEST_MESSAGE( "generating genomic variants (using substitution frequencies)..." );
  vector<Variant> variants = generateVariants(1000, ref_genome, model, rng);
  BOOST_TEST_MESSAGE( "done generating 1000 variants, here are the first 10: " );
  BOOST_TEST_MESSAGE( "  id, chr, bp, ref, alt" );
  for (int i=0; i<10; ++i) {
    BOOST_TEST_MESSAGE( format("%s,%s,%d,%s,%s") % variants[i].id % variants[i].chr % variants[i].pos % variants[i].alleles[0] % variants[i].alleles[1] );
  }

  // export variants to file
  vector<string> labels = { "healthy" };
  // create mutation matrix (contains info about which variants apply and to which chr copy)
  vector<vector<short>> mutMatrix(1, vector<short>(variants.size(), 1));
  for (auto i=0; i<variants.size(); ++i)
    mutMatrix[0][i] += variants[i].chr_copy;

  auto fn_out = "ref_variants.vcf";
  ofstream fs_out;
  fs_out.open(fn_out);
  vario::writeVcf(ref_genome.records, variants, labels, mutMatrix, fs_out);
  fs_out.close();
  BOOST_TEST_MESSAGE( " variants are in file '" << fn_out << "'." );


  BOOST_TEST_MESSAGE( "creating diploid reference genome:" );
  BOOST_TEST_MESSAGE( "  applying variants to reference..." );

}

/* compare distributions for variants generated
 *  A) based on mutated nucleotide frequencies from VCF
 *  B) when positions are picked randomly
 */
BOOST_AUTO_TEST_CASE( var_dist )
{
  boost::timer::auto_cpu_timer t;
  BOOST_TEST_MESSAGE( "generating 10000 genomic variants (using substitution frequencies)..." );
  vector<Variant> vars_model = generateVariants(10000, ref_genome, model, rng);
  VariantSet varset_model;
  varset_model.vec_variants = vars_model;
  varset_model.calculateSumstats();
  double (&f)[4][4] = varset_model.mat_freqs;
  BOOST_TEST_MESSAGE( "\tnucleotide substitution frequencies:" );
  BOOST_TEST_MESSAGE( "   |    A   |    C   |    G   |    T   " );
  BOOST_TEST_MESSAGE( format(" A | %0.4f | %0.4f | %0.4f | %0.4f ") % f[0][0] % f[0][1] % f[0][2] % f[0][3] );
  BOOST_TEST_MESSAGE( format(" C | %0.4f | %0.4f | %0.4f | %0.4f ") % f[1][0] % f[1][1] % f[1][2] % f[1][3] );
  BOOST_TEST_MESSAGE( format(" G | %0.4f | %0.4f | %0.4f | %0.4f ") % f[2][0] % f[2][1] % f[2][2] % f[2][3] );
  BOOST_TEST_MESSAGE( format(" T | %0.4f | %0.4f | %0.4f | %0.4f ") % f[3][0] % f[3][1] % f[3][2] % f[3][3] );

  BOOST_TEST_MESSAGE( "generating 10000 genomic variants (random positions)..." );
  vector<Variant> vars_random = generateVariantsRandomPos(10000, ref_genome, model, rng);
  VariantSet varset_random;
  varset_random.vec_variants = vars_random;
  varset_random.calculateSumstats();
  double (&g)[4][4] = varset_random.mat_freqs;
  BOOST_TEST_MESSAGE( "\tnucleotide substitution frequencies:" );
  BOOST_TEST_MESSAGE( "   |    A   |    C   |    G   |    T   " );
  BOOST_TEST_MESSAGE( format(" A | %0.4f | %0.4f | %0.4f | %0.4f ") % g[0][0] % g[0][1] % g[0][2] % g[0][3] );
  BOOST_TEST_MESSAGE( format(" C | %0.4f | %0.4f | %0.4f | %0.4f ") % g[1][0] % g[1][1] % g[1][2] % g[1][3] );
  BOOST_TEST_MESSAGE( format(" G | %0.4f | %0.4f | %0.4f | %0.4f ") % g[2][0] % g[2][1] % g[2][2] % g[2][3] );
  BOOST_TEST_MESSAGE( format(" T | %0.4f | %0.4f | %0.4f | %0.4f ") % g[3][0] % g[3][1] % g[3][2] % g[3][3] );
}

BOOST_AUTO_TEST_SUITE_END()