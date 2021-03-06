/**
 * A remake of the R2KS code, optimised for the apocrita cluster
 * Based on the paper by Ni and Vingron
 * http://pubman.mpdl.mpg.de/pubman/item/escidoc:1702988/component/escidoc:1711821/Ni.pdf
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <list>
#include <cstdlib>
#include <getopt.h>
#include <time.h>
#include <sys/time.h>

#ifdef USE_MPI
#include <mpi.h>
#endif

#include <omp.h>
#include <sstream>

using namespace std;


// Options Struct
struct Options {
  std::string filename;

  // Internal
  unsigned int  num_genes;
  unsigned int  num_lists;
  unsigned int  pivot;
  bool          two_tailed;

  // MPI
  int num_procs, mpi_id;

};

#ifdef USE_MPI
// MPI Datatype
typedef struct {
  int i,j;
  double result;
}MPIResult;

MPI_Datatype resultType;
#endif

/**
 * Calculate the weight for this term
 * For now, we are ignoring weight
 */

double calculateWeight(unsigned int idx, unsigned int pivot) {
  if (pivot == 0)
    return 1.0;
  double h = static_cast<double>(pivot) - static_cast<double>(idx);
  double w = h < 0.0 ? 1.0 : h * (h + 1.0) / 2.0;  
  return w;
}


/**
 * Perform the r2ks score for two lists
 * Take the two lists, the corresponding weights and the total weight, then perform
 * the scoring.
 */

double scoreLists(Options & options, std::vector<unsigned int> & gene_list0, std::vector<unsigned int> & gene_list1) {


  // First, calculate total weight
  // TODO - we can so cache this

  // TODO - Given how weights are calculated this formula might work
  // totalweight = (pivot * ( pivot + 1) * (pivot + 2)) / 6.0 + (list length - pivot);

  float total_weight = 0.0;
  for (unsigned int i = 0; i < gene_list0.size(); ++i ){
    //cout << "weight " <<  calculateWeight(i, options.pivot) << endl;
    total_weight += calculateWeight(i, options.pivot);
  }

  std::vector<unsigned int> buff (gene_list0.size());

  for (unsigned int i = 0; i < gene_list0.size(); ++i ){
    buff[ gene_list1[i] ] = i;
  }

  // The algorithm follows the paper:
  // http://online.liebertpub.com/doi/pdf/10.1089/cmb.2012.0026
  // We optimise the algorithm by storing a history of previous values in the second loop
  // We take advantage of the fact that a gene is unique and always occurs in both lists and only once
  // thus rather than filling in the complete matrix, we only fill in the values we need by keeping
  // a record of where and what the previous values were.

  typedef struct {
    unsigned int pos_y;
    double value;
  }History;

  // Used to calculate the actual score
  double rvalue = 0.0;
  double one_over = 1.0 /  (options.num_genes * options.num_genes);

  std::vector<History> history;


  // First line has no history, so we do this one separately.
  {

    unsigned int ivalue = gene_list0[0];
    float pivot = buff[ivalue];

    double w = calculateWeight(0, options.pivot);
    //double jw = calculateWeight(pivot, options.pivot);
    //double w = iw < jw ? iw : jw;
    double front = w;

    double prev = 0.0;

    History nh;
    nh.value = front;
    nh.pos_y = pivot;

    history.push_back(nh);

  }

  
  // Now we have a history so we perform the operation normally

  for (unsigned int i = 1; i < options.num_genes; ++i ){

    unsigned int ivalue = gene_list0[i];
    float pivot = buff[ivalue];

    double iw = calculateWeight(i, options.pivot);
    double jw = calculateWeight(pivot, options.pivot);
    double w = iw < jw ? iw : jw;
    
    double front = 0.0;

    History lh = history[history.size()-1];

    // we check to see if the pivot is still greater than all the histories
    // This is the best case scenario

    if (pivot > lh.pos_y){
      History nh;
      nh.value = lh.value + w;
      nh.pos_y = pivot;
      history.push_back(nh);

      double second_term = static_cast<double>((nh.pos_y+1) * (i+1)) * one_over;
      double nvalue = (nh.value / total_weight) - second_term;
      rvalue = nvalue > rvalue ? nvalue : rvalue; 
    
    } else {

      std::vector<History>::iterator it = history.end();

      // we need to trawl back through the histories, updating as we go
    
      for (it--; it != history.begin() && it->pos_y > pivot; it--){

        if (it->pos_y > pivot){
          it->value += w;
          double second_term = static_cast<double>((it->pos_y+1) * (i+1)) * one_over;
          double nvalue = (it->value / total_weight) - second_term;
          rvalue = nvalue > rvalue ? nvalue : rvalue; 
        } 
      }
      
      // Insert new pivot
      History nh;
      nh.value = it->value + w;
      nh.pos_y = pivot;

      history.insert(it+1, nh);

      double second_term = static_cast<double>((nh.pos_y+1) * (i+1)) * one_over;
      double nvalue = (nh.value / total_weight) - second_term;
      rvalue = nvalue > rvalue ? nvalue : rvalue; 

    }

  }

  return rvalue * sqrt(options.num_genes);
}


/**
 * Read the header block from our file
 */

void readHeaderBlock(Options & options) {
  std::ifstream is (options.filename.c_str());
  is >> options.num_genes >> options.num_lists;
  is.close();
}


/**
 * Read a line from a file.
 * Assume gene_list is pre-populated with zeros
 */

void readLineIndex(Options & options, int idx, std::vector<unsigned int> & gene_list ) {  

  std::ifstream fin;
  fin.open(options.filename.c_str());
  fin.seekg(std::ios::beg);

  for(int i=0; i < idx; ++i){
    fin.ignore(std::numeric_limits<std::streamsize>::max(),'\n');
  }

  // The list we are reading in is a set of gene expressions.
  // The position/index of this number is the gene itself.
  // We want a list that ranks these indices

  unsigned int gene_expression;
  unsigned int gidx = 0;
  while (fin >> gene_expression && gidx < options.num_genes) {
    gene_list[gene_expression] = gidx;
    gidx++;
  }

  fin.close();
}


template<class T> inline T FromStringS9(const std::string& s) {
  std::istringstream stream (s);
  T t;
  stream >> t;
  return t;
}


/**
 * parse our command line options
 */

void parseCommandOptions (int argc, const char * argv[], Options &options) {
  int c;
  int digit_optind = 0;
  static struct option long_options[] = {
  };

  int option_index = 0;

  while ((c = getopt_long(argc, (char **)argv, "f:w:t?", long_options, &option_index)) != -1) {
    int this_option_optind = optind ? optind : 1;
    switch (c) {
      case 0 :
        break;

      case 'w' :
        options.pivot = FromStringS9<unsigned int>(std::string(optarg));
        break;

      case 't' :
        options.two_tailed = true;
        break;
  
      case 'f' :
        options.filename = std::string(optarg);
        break;

      default:
        std::cout << "?? getopt returned character code" << c << std::endl;
      }
  }
  
  if (optind < argc) {
      std::cout << "non-option ARGV-elements: " << std::endl;
      while (optind < argc)
          std::cout << argv[optind++];
      std::cout << std::endl;
  }

}


#ifdef USE_MPI 

/**
 * Master MPI Process
 */

void masterProcess(Options &options){
  int total_tests =  options.num_lists * ( options.num_lists - 1) / 2;
  int processes_per_node = total_tests / (options.num_procs - 1);
  int extra_processes = total_tests % (options.num_procs - 1);

  vector < int > test_numbers;

  // Create our list of pairs
  for (int i=1; i < options.num_lists + 1; ++i){
    for (int j = i; j < options.num_lists + 1; ++j){
      test_numbers.push_back( i );
      test_numbers.push_back( j );
    }
  }


  // Send the initial values to each client process, its total number of tests
  // and the test numbers themselves

  for (int i = 1; i < options.num_procs; ++i){

    int send_count = processes_per_node;
    if (i == options.num_procs - 1)
      send_count += extra_processes;

    int offset = processes_per_node * 2 * (i-1);
 
    send_count *= 2;
    MPI_Send(&send_count, 1, MPI_INT, i, 999, MPI_COMM_WORLD);
    MPI_Send(&test_numbers[offset], send_count, MPI_INT, i, 999, MPI_COMM_WORLD);

  } 

  // Now wait to receive the results
  unsigned int results = 0;

  while(results < total_tests){
    MPIResult mp;
    MPI_Status status;
    MPI_Recv(&mp, 1, resultType, MPI_ANY_SOURCE,  MPI_ANY_TAG,MPI_COMM_WORLD, &status );
    results++;
    std::cout << mp.i << "_" << mp.j << " " << mp.result << std::endl;
  }


}


/**
 * Client MPI Process
 */

void clientProcess(Options &options){

  MPI_Status status;
  int bsize;

  MPI_Recv(&bsize, 1, MPI_INT, 0, MPI_ANY_TAG,MPI_COMM_WORLD, &status);
  
  int buffer[bsize];
  MPI_Recv(&buffer, bsize, MPI_INT, 0, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

  // Now the proper loop begins

  for (int i = 0; i < bsize; i+=2){
    int l0 = buffer[i];
    int l1 = buffer[i+1];

  
    std::vector<unsigned int> gene_list0(options.num_genes);
    std::vector<unsigned int> gene_list1(options.num_genes);

    readLineIndex(options, l0, gene_list0);
    readLineIndex(options, l1, gene_list1);

    float rvalue = scoreLists(options, gene_list0, gene_list1 );

    // For a two-tailed test reverse the order of the second list and compare
    if (options.two_tailed){
      std::reverse(gene_list1.begin(), gene_list1.end());
      float tvalue = scoreLists(options, gene_list0, gene_list1 );
      rvalue = tvalue > rvalue ? tvalue : rvalue;
    }

    MPIResult mp;
    mp.i = l0;
    mp.j = l1;
    mp.result = rvalue;

    MPI_Send(&mp, 1, resultType, 0, 999, MPI_COMM_WORLD);

  }

}

#endif

/**
 * Main entry point
 */

int main (int argc, const char * argv[]) {
  Options ops;
 
  // Defaults for Options
  ops.pivot = 0;
  ops.two_tailed = false;
  parseCommandOptions(argc,argv,ops);
 
#ifdef USE_MPI

  // MPI Init

  struct timeval start,end; 

  MPI_Init(&argc, const_cast<char***>(&argv));
  int length_name;
  char name[200];

  MPI_Comm_size(MPI_COMM_WORLD, &ops.num_procs);
  MPI_Comm_rank(MPI_COMM_WORLD, &ops.mpi_id);
  MPI_Get_processor_name(name, &length_name);
  
  //if (ops.mpi_id == 0){
  //  cout << "MPI NumProcs: " << ops.num_procs << endl;
  //}
  
  //cout << "MPI ID: " << ops.mpi_id << " Name: " << name << endl;

  /// MPI Datatype for results
  MPI_Aint offsets[2], extent; 
  MPI_Datatype oldtypes[2];
  int blockcounts[2]; 

  offsets[0] = 0; 
  oldtypes[0] = MPI_INT; 
  blockcounts[0] = 2; 
 
  MPI_Type_extent(MPI_INT, &extent); 
  offsets[1] = 2 * extent; 
  oldtypes[1] = MPI_DOUBLE; 
  blockcounts[1] = 1; 

  MPI_Type_struct(2, blockcounts, offsets, oldtypes, &resultType); 
  MPI_Type_commit(&resultType); 


  // Read only, so mpi processes shouldnt clash with that
  readHeaderBlock(ops);

  gettimeofday(&start,NULL);  

  if (ops.num_procs > 1){

    // The master MPI Process will farm out to the required processes
    if (ops.mpi_id == 0){
      masterProcess(ops);

    } else {
      clientProcess(ops);
    }

  }
  MPI_Finalize();
  gettimeofday(&end,NULL);
  
  double dif = ((double)end.tv_sec + (double)end.tv_usec * .000001) -
    ((double)start.tv_sec + (double)start.tv_usec * .000001);

  cout << "Wall clock time: " << dif << endl; 
 
#endif

#ifndef USE_MPI

  readHeaderBlock(ops);

  double start, end;
  start = omp_get_wtime(); 
 
  cout << "Running with OpenMP with " <<  omp_get_num_threads() << " threads" <<  endl;
  
  {
    
#pragma omp parallel for
    for (int i = 0; i < ops.num_lists; ++i ){
         
      for (int j = i + 1; j < ops.num_lists; ++j ){ 
        int l0 = i + 1;
        int l1 = j + 1;
        
        std::vector<unsigned int> gene_list0(ops.num_genes);
        std::vector<unsigned int> gene_list1(ops.num_genes);

        readLineIndex(ops, l0, gene_list0);
        readLineIndex(ops, l1, gene_list1);

        float rvalue = scoreLists(ops, gene_list0, gene_list1 );

        if (ops.two_tailed){
          std::reverse(gene_list1.begin(), gene_list1.end());
          float tvalue = scoreLists(ops, gene_list0, gene_list1 );
          rvalue = tvalue > rvalue ? tvalue : rvalue;
        }

        cout << l0 << "_" << l1 << " " << rvalue << endl;
        
      }
    }
  }
  
  end = omp_get_wtime();
  cout << "Wall clock time: " << end - start << endl; 
   

#endif
  return 0;

}
