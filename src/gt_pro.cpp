#if __linux__
#include <linux/version.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,22)
#define _MAP_POPULATE_AVAILABLE
#endif
#endif

#ifdef _MAP_POPULATE_AVAILABLE
#define MMAP_FLAGS (MAP_PRIVATE | MAP_POPULATE)
#else
#define MMAP_FLAGS (MAP_PRIVATE)
#endif

#include <sys/mman.h>
#include <stdio.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <assert.h>
#include <string.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <iostream>
#include <sstream>
#include <fstream>
#include <thread>
#include <limits>
#include <libgen.h>
#include <regex>

using namespace std;

constexpr auto step_size = 32 * 1024 * 1024;
constexpr auto buffer_size = 32 * 1024 * 1024;

// The DB k-mers are 31-mers.
constexpr auto K = 31;

// 2 bits to encode each ACTG letter
constexpr auto BITS_PER_BASE = 2;

// number of bits to encode entire K-mer
constexpr auto K2 = BITS_PER_BASE * K;

constexpr uint64_t LSB = 1;
constexpr uint64_t BIT_MASK = (LSB << (K * BITS_PER_BASE)) - LSB;

// Choose appropriately sized integer types to represent offsets into
// the database.
using LmerRange = uint64_t;
constexpr auto START_BITS = 48;
constexpr auto LEN_BITS = 64 - START_BITS;
constexpr auto MAX_START = (LSB << START_BITS) - LSB;
constexpr auto MAX_LEN = (LSB << LEN_BITS) - LSB;

// element 0, 1:  61-bp nucleotide sequence centered on SNP, for a more detailed map please see
// the comment "note on the binary representation of nucleotide sequences" below.
// element 2:  SNP coordinates consisting of species ID, major/minor allele bit, and genomic position.
using SNPRepr = tuple<uint64_t, uint64_t, uint64_t>;

// This param is only useful for perf testing.  The setting below, not
// to exceed 64 TB of RAM, is equivalent to infinity in 2019.
constexpr auto MAX_MMAP_GB = 64*1024;
constexpr auto MAX_END = MAX_MMAP_GB * (LSB << 30) / 8;

size_t get_fsize(const char* filename) {
	struct stat st;
	if (stat(filename, &st) == -1) {
		// Probably file not found.
		return 0;
	}
	return st.st_size;
}

struct CodeDict {
    vector<uint8_t> code_dict;
    uint8_t* data;
    CodeDict() {
        constexpr auto CHAR_LIMIT = 1 << (sizeof(char) * 8);
        for (uint64_t c = 0; c < CHAR_LIMIT; ++c) {
            // This helps us detect non-nucleotide characters on encoding.
            code_dict.push_back(0xff);
        }
        code_dict['A'] = code_dict['a'] = 0;
        code_dict['C'] = code_dict['c'] = 1;
        code_dict['G'] = code_dict['g'] = 2;
        code_dict['T'] = code_dict['t'] = 3;
        data = code_dict.data();
    }
};
const CodeDict code_dict;

// see the comment "note on the binary representation of nucleotide sequences" below.
template <class int_type, int len>
int_type seq_encode(const char* buf) {
	int_type seq_code = 0;
	// This loop may be unrolled by the compiler because len is a compile-time constant.
	for (int bitpos = 0;  bitpos < len;  bitpos += BITS_PER_BASE) {
		const uint8_t b_code = code_dict.data[*buf++];
		assert((b_code & 0xfc) == 0);
		seq_code |= (((int_type) b_code) << bitpos);
	}
	return seq_code;
}

long chrono_time() {
	using namespace chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

template <int M2, int M3>
bool kmer_lookup_work(LmerRange* lmer_index, uint64_t* mmer_bloom, uint32_t* kmers_index, SNPRepr* snps, int channel, char* in_path, char* o_name, int aM2, int aM3) {

	if (aM2 != M2 || aM3 != M3) {
		return false;
	}

	const uint64_t MAX_BLOOM = (LSB << M3) - LSB;
    constexpr int XX = (M3 + 1) / 2;  // number DNA letters to cover MAX_BLOOM

	auto out_path = string(o_name) + "." + to_string(channel) + ".tsv";

	//Matching: lmer table lookup then linear search
	vector<char> buffer(buffer_size);
	char* window = buffer.data();

	uint64_t n_lines = 0;

	//  Print progress update every 5 million lines.
	constexpr uint64_t PROGRESS_UPDATE_INTERVAL = 5 * 1000 * 1000;

	// Reads that contain wildcard characters ('N' or 'n') are split into
	// tokens at those wildcard characters.  Each token is processed as
	// though it were a separate read.
	constexpr int MAX_TOKEN_LENGTH = 500;
	constexpr int MIN_TOKEN_LENGTH = 31;

	char seq_buf[MAX_TOKEN_LENGTH];

	// This ranges from 0 to the length of the longest read (could exceed MAX_TOKEN_LENGTH).
	int token_length = 0;

	vector<uint64_t> kmer_matches;

	unordered_map<uint64_t, int> footprint;

	int fd = open(in_path, O_RDONLY);

	auto s_start = chrono_time();
	char c = '\0';

	while (true) {
		const ssize_t bytes_read = read(fd, window, step_size);

		if (bytes_read == 0)
			break;

		if (bytes_read == (ssize_t) - 1) {
			cerr << chrono_time() << ":  " << "unknown fatal error, when read stdin input" << endl;
			exit(EXIT_FAILURE);
		}

		for (uint64_t i = 0;  i < bytes_read;  ++i) {

			if (c == '\n') {
				++n_lines;
				if ((n_lines + 1) % PROGRESS_UPDATE_INTERVAL == 0) {
					cerr << chrono_time() << ":  " << ((n_lines + 3) / 4) << " reads were scanned after "
						 << (chrono_time() - s_start) / 1000 << " seconds from file "
						 << in_path << endl;
				}
			}

			// Invariant:  The number of new line characters consumed before window[i]
			// is the value of n_lines.

			c = window[i];

			// In FASTQ format, every 4 lines define a read.  The first line is the
			// read header.  The next line is the read sequence.  We only care about
			// the read sequence, where n_lines % 4 == 1.
			if (n_lines % 4 != 1) {
				// The current line does *not* contain a read sequence.
				// Next character, please.
				continue;
			}

			// The current line contains a read sequence.  Split it into tokens at wildcard 'N'
			// characters.  Buffer current token in seq_buf[0...MAX_READ_LENGTH-1].
			const bool at_token_end = (c == '\n') || (c == 'N') || (c == 'n');
			if (!(at_token_end)) {
				// Invariant:  The current token length is token_length.
				// Only the first MAX_TOKEN_LENGTH charaters of the token are retained.
				if (token_length < MAX_TOKEN_LENGTH) {
					seq_buf[token_length] = c;
				}
				++token_length;
				// next character, please
				continue;
			}

			// is token length within acceptable bounds?   if not, token will be dropped silently
			if (MIN_TOKEN_LENGTH <= token_length && token_length <= MAX_TOKEN_LENGTH) {

				// yes, process token
				for (int j = 0;  j <= token_length - K;  ++j) {

					const auto mmer_pres = seq_encode<uint64_t, XX>(seq_buf + j + K - XX) & MAX_BLOOM;
					const auto mpres = mmer_bloom[mmer_pres / 64] >> (mmer_pres % 64);

					if (mpres & 1) {
						const auto kmer = seq_encode<uint64_t, K>(seq_buf + j);
						const uint32_t lmer = kmer >> M2;
						const auto range = lmer_index[lmer];
						const auto start = range >> LEN_BITS;
						const auto end = min(MAX_END, start + (range & MAX_LEN));

						for (uint64_t z = start;  z < end;  ++z) {
							const auto kmi = kmers_index[z];
							const auto offset = kmi & 0x1f;
							const auto snp_id = kmi >> 5;
							const auto& snps_repr = snps[snp_id];
							const auto low_bits = get<0>(snps_repr) >> (62 - (offset * BITS_PER_BASE));
							const auto high_bits = get<1>(snps_repr) << (offset * BITS_PER_BASE);
							const auto db_kmer = (high_bits | low_bits) & BIT_MASK;
							if (kmer == db_kmer) {
								if (footprint.find(snp_id) == footprint.end()) {
									kmer_matches.push_back(snp_id);
									footprint.insert({snp_id, 1});
								}
							} else if (kmer < db_kmer) {
								break;
							}
						}
					}
				}
			}

			// clear footprint for every read instead of every token
			if(c == '\n') {
				footprint.clear();
			}

			// next token, please
			token_length = 0;
		}
	}

	if (token_length != 0) {
		cerr << chrono_time() << ":  " << "Error:  Truncated read sequence at end of file: " << in_path << endl;
		exit(EXIT_FAILURE);
	}

	cerr << chrono_time() << ":  " << "[Done] searching is completed, emitting results for " << in_path << endl;
	ofstream fh(out_path, ofstream::out | ofstream::binary);

	if (kmer_matches.size() == 0) {
		cerr << chrono_time() << ":  " << "zero hits" << endl;
	} else {
		for (int i=0;  i<kmer_matches.size();  ++i) {
			kmer_matches[i] = get<2>(snps[kmer_matches[i]]);
		}
		sort(kmer_matches.begin(), kmer_matches.end());
		// FIXME.  The loop below doesn't output the last SNP.
		// Also the counts may be off by 1.
		uint64_t cur_count = 0;
		uint64_t cur_snp = kmer_matches[0];
		for (auto it : kmer_matches) {
			if (it != cur_snp) {
				fh << cur_snp << '\t' << cur_count << '\n';
				cur_snp = it;
				cur_count = 1;
			} else {
				++cur_count;
			}
		}
	}
	cerr << chrono_time() << ":  " << "Completed output for " << in_path << endl;

	fh.close();

	return true;
}


void kmer_lookup(LmerRange* lmer_index, uint64_t* mmer_bloom, uint32_t* kmers_index, SNPRepr* snps, int channel, char* in_path, char* o_name, int M2, const int M3) {
	// Only one of these will really run.  By making them known at compile time, we increase speed.
	// The command line params corresponding to these options are L in {26, 27, 28, 29, 30}  x  M in {30, 32, 34, 35, 36, 37}.
	bool match = (
			    kmer_lookup_work<32, 30>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<32, 32>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<32, 34>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<32, 35>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<32, 36>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<32, 37>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)

	         || kmer_lookup_work<33, 30>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<33, 32>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<33, 34>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<33, 35>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<33, 36>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<33, 37>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)

	         || kmer_lookup_work<34, 30>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<34, 32>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<34, 34>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<34, 35>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<34, 36>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<34, 37>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)

	         || kmer_lookup_work<35, 30>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<35, 32>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<35, 34>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<35, 35>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<35, 36>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<35, 37>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)

	         || kmer_lookup_work<36, 30>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<36, 32>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<36, 34>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<36, 35>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<36, 36>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	         || kmer_lookup_work<36, 37>(lmer_index, mmer_bloom, kmers_index, snps, channel, in_path, o_name, M2, M3)
	);
	assert(match && "See comment for supporrted values of L and M.");
}

void display_usage(char* fname){
	cout << "usage: " << fname << " -d <sckmerdb_path: string> -t <n_threads; int; default 1> -o <out_prefix; string; default: cur_dir/out> [-h] input1 [input2 ...]\n";
}


template <class ElementType>
struct DBIndex {

	string filename;
	bool loaded_or_mmapped;

	DBIndex(const string& filename, const uint64_t expected_element_count=0)
		: filename(filename),
		  mmapped_data(NULL),
		  loaded_or_mmapped(false),
		  expected_element_count(expected_element_count),
		  fd(-1),
		  filesize(0)
	{}

	ElementType* address() {
		if (mmapped_data) {
			return mmapped_data;
		}
		assert(elements.size() > 0);
		return &(elements[0]);
	}

	vector<ElementType>* getElementsVector() {
		return &(elements);
	}

	// If file exists and nonempty, preload or mmap depending on argument, and return false.
	// If file is missing or empty, allocate space in elements array and return true.
	bool mmap_or_load(const bool preload = false) {
		assert(!(loaded_or_mmapped));
		filesize = get_fsize(filename.c_str());
		if (filesize) {
			if (!(expected_element_count)) {
				expected_element_count = filesize / sizeof(ElementType);
			}
			assert(filesize == expected_element_count * sizeof(ElementType));
			if (preload) {
				load();
			} else {
				MMAP();
			}
		}
		if (loaded_or_mmapped) {
			// Does not need to be recomputed.
			return false;
		}
		cerr << chrono_time() << ":  Failed to MMAP or preload " << filename << ".  This is fine, but init will be slower as we recreate this file." << endl;
		elements.resize(expected_element_count);
		// needs to be recomputed
		return true;
	}

	void save() {
		assert(!(loaded_or_mmapped));
		auto l_start = chrono_time();
		FILE* dbout = fopen(filename.c_str(), "wb");
		assert(dbout);
		const auto saved_element_count = fwrite(address(), sizeof(ElementType), elements.size(), dbout);
		fclose(dbout);
		assert(saved_element_count == elements.size());
		cerr << chrono_time() << ":  Done writing " << filename << ". That took " << (chrono_time() - l_start) / 1000 << " more seconds." << endl;
	}

	~DBIndex() {
		if (fd != -1) {
			assert(mmapped_data);
			assert(loaded_or_mmapped);
			int rc = munmap(mmapped_data, filesize);
			assert(rc == 0);
			close(fd);
		}
	}

private:
	vector<ElementType> elements;
	ElementType* mmapped_data;
	uint64_t expected_element_count;
	int fd;
	uint64_t filesize;

	void load() {
		FILE* dbin = fopen(filename.c_str(), "rb");
		if (dbin) {
			cerr << chrono_time() << ":  Loading " << filename << endl;
			elements.resize(expected_element_count);
			const auto loaded_element_count = fread(address(), sizeof(ElementType), expected_element_count, dbin);
			if (loaded_element_count == expected_element_count) {
				cerr << chrono_time() << ":  Loaded " << filename << endl;
				loaded_or_mmapped = true;
			}
			fclose(dbin);
		}
	}

	void MMAP() {
		fd = open(filename.c_str(), O_RDONLY, 0);
		if (fd != -1) {
			cerr << chrono_time() << ":  MMAPPING " << filename << endl;
			auto mmappedData = (ElementType *) mmap(NULL, filesize, PROT_READ, MMAP_FLAGS, fd, 0);
			if (mmappedData != MAP_FAILED) {
				mmapped_data = mmappedData;
				loaded_or_mmapped = true;
				cerr << chrono_time() << ":  MMAPPED " << filename << endl;
			}
		}
	}
};


struct SNPSeq {
	uint64_t low_64;
	uint64_t high_64;
};


uint32_t kmer_repr(uint64_t kmer, uint64_t snp_coords, uint32_t snp_id, SNPSeq snp_seq) {
}


int main(int argc, char** argv) {
	extern char *optarg;
	extern int optind;

	bool dbflag = false;
	bool inflag = false;

	char* fname = argv[0];
	char* db_path = (char *)"";
	char* oname = (char *)"./out";

	// Number of bits in the prefix part of the K-mer (also called L-mer,
	// even though it might not correspond to an exact number of bases).
	// Override with command line -l parameter.
	//
	// This has a substantial effect on memory use.  Rule of thumb for
	// perf is L2 >= K2 - M3.  However, that rule may be broken in order
	// to reduce RAM use and eliminate I/O which is even worse for perf.
	auto L2 = 29;

	// Number of bits in the MMER_BLOOM index.  This has a substantial effect
	// on memory use.  Rule of thumb for perf is M3 >= 4 + log2(DB cardinality).
	// Override with command line -m parameter.
	auto M3 = 36;

	int n_threads = 1;

	auto preload = false;

	int opt;
	while ((opt = getopt(argc, argv, "pl:m:d:t:o:h")) != -1) {
		switch (opt) {
			case 'd':
				dbflag = true;
				db_path = optarg;
				break;
			case 't':
				n_threads = stoi(optarg);
				break;
			case 'o':
				oname = optarg;
				break;
			case 'l':
				L2 = stoi(optarg);
				break;
			case 'm':
				M3 = stoi(optarg);
				break;
			case 'p':
				preload = true;
				break;
			case 'h': case '?':
				display_usage(fname);
				exit(1);
		}
	}

	const auto M2 = K2 - L2;

	assert(L2 > 0);
	assert(L2 <= 32);
	assert(M2 > 0);
	assert(M2 < 64);
	assert(M3 > 0);
	assert(M3 < 64);

	// FIXME this is just for now
	assert(L2 == 30 &&  "Sorry, for now only -l 30 is supported." );
	assert(M2 == 32);

	const auto LMER_MASK = (LSB << L2) - LSB;
	const auto MMER_MASK = (LSB << M2) - LSB;
	const auto MAX_BLOOM = (LSB << M3) - LSB;

	cout << fname << '\t' << db_path << '\t' << n_threads << "\t" << (preload ? "preload" : "mmap") << "\t" << L2 << "\t" << M3 << endl;

	if (!dbflag) {
		cout << "missing argument: -d <sckmerdb_path: string>\n";
		display_usage(fname);
		exit(1);
	}

	int in_pos = optind;
	if (optind == argc) {
		cout << "missing argument: input (>1)\n";
		display_usage(fname);
		exit(1);
	}

	auto l_start = chrono_time();
	cerr << chrono_time() << ":  " << "Starting to load DB: " << db_path << endl;

	size_t db_filesize = get_fsize(db_path);

	string dbbase = string(basename(db_path));
	dbbase = regex_replace(dbbase, regex("\\.bin$"), "");
	dbbase = regex_replace(dbbase, regex("\\."), "_");

	if (preload) {  // force preload, probably unnecessary and wasteful
		cerr << chrono_time() << ":  DB indexes will be preloaded." << endl;
	}

	// The input (un-optimized) DB is a sequence of 56-bit snp followed by 8-bit offset
	// of SNP within kmer followed by 64-bit kmer.  The 56-bit snp encodes the species id,
	// major/minor allele, and genomic position.  From that we build the "optimized" DBs
	// below.  The first one, db_snps, lists the unique SNPs in arbitrary order; and
	// for each SNP in addition to the 56-bits mentioned above it also shows the
	// sequence of 61bp centered on the SNP inferred from all kmers in the original DB.
    // This needs to be explained a little better;  see email (eventually docs).
	DBIndex<SNPRepr> db_snps(dbbase + "_optimized_db_snps.bin");
	const bool recompute_snps = db_snps.mmap_or_load(preload);
	vector<SNPRepr>& snps = *db_snps.getElementsVector();

	// This encodes a list of all kmers, sorted in increasing order.  Each kmer is represented
	// not by the 62 bits of its 31-bp nucleotide sequence but rather by 27-bits that represent
	// an index into the db_snps table above, and 5 bits representing the SNP position within
	// the kmer.  This needs to be explained a little better;  see email (eventually docs).
	DBIndex<uint32_t> db_kmer_index(dbbase + "_optimized_db_kmer_index_" + to_string(M2) + ".bin");
	const bool recompute_kmer_index = db_kmer_index.mmap_or_load(preload);
	vector<uint32_t>& kmer_index = *db_kmer_index.getElementsVector();

	// Bit vector with one presence/absence bit for every possible M3-bit kmer suffix (the M3
	// LSBs of a kmer's nucleotide sequence).
	DBIndex<uint64_t> db_mmer_bloom(
		dbbase + "_optimized_db_mmer_bloom_" + to_string(M3) + ".bin",
		(1 + MAX_BLOOM) / 64
	);
	const bool recompute_mmer_bloom = db_mmer_bloom.mmap_or_load(preload);
	uint64_t* mmer_bloom = db_mmer_bloom.address();

	// For every kmer in the original DB, the first L2 bits of the kmer's nucleotide sequence
	// are called that kmer's lmer.  Kmers that share the same lmer occupy a range of consecutive
	// positions in the kmer_index, and that range is lmer_index[lmer].
	DBIndex<LmerRange> db_lmer_index(
		dbbase + "_optimized_db_lmer_index_" + to_string(L2) + ".bin",
		1 + LMER_MASK
	);
	const bool recompute_lmer_index = db_lmer_index.mmap_or_load(preload);
	LmerRange* lmer_index = db_lmer_index.address();

	assert(recompute_kmer_index == recompute_snps && "Please delete both optimized_db_kmers_index.bin and optimized_db_snps.bin.");
	const bool recompute_kmer_index_and_snps = recompute_kmer_index || recompute_snps;

	uint64_t last_lmer;
	uint64_t start = 0;

	uint64_t lmer_count = -1;

	int fd = -1;
	uint64_t* db_data = NULL;

	if (recompute_mmer_bloom || recompute_lmer_index || recompute_kmer_index_and_snps) {
		//Open file
		fd = open(db_path, O_RDONLY, 0);
		assert(fd != -1);
		db_data = (uint64_t *) mmap(NULL, db_filesize, PROT_READ, MMAP_FLAGS, fd, 0);
		assert(db_data != MAP_FAILED);
		unordered_map<uint64_t, uint32_t> snps_map;
		vector<tuple<uint64_t, uint64_t>> snps_known_bits;  // not persisted, just for integrity checking during construction
		lmer_count = db_filesize ? 1 : 0;
		for (uint64_t end = 0;  end < 20*1000*1000/*db_filesize / 8*/;  end += 2) {
			const auto kmer = db_data[end + 1];
			const auto lmer = kmer >> M2;
			if (recompute_mmer_bloom) {
				const uint64_t bloom_index = kmer & MAX_BLOOM;
				mmer_bloom[bloom_index / 64] |= ((uint64_t) 1) << (bloom_index % 64);
			}
			if (recompute_kmer_index_and_snps) {
				const auto snp_with_offset = db_data[end];
				const auto snp = snp_with_offset >> 8;
				const auto offset = snp_with_offset & 0xff;
				const auto map_entry = snps_map.find(snp);
				uint32_t snp_id;
				if (map_entry == snps_map.end()) {
					snp_id = snps_map.size();
					snps_map.insert({snp, snp_id});
					snps.push_back(SNPRepr(0, 0, snp));
					snps_known_bits.push_back(tuple<uint64_t, uint64_t>(0,0));
				} else {
					snp_id = map_entry->second;
				}
				assert(0 <= offset && offset < K && offset <= 31);
				assert(snp_id < (1 << 27) && "Too many SNPs in database.");
				const auto kmer_repr = (snp_id << 5) || offset;
				kmer_index.push_back(kmer_repr);
				auto &snp_repr = snps[snp_id];
				auto &snp_known_bits_mask = snps_known_bits[snp_id];
				//
				// A note on the binary representation of nucleotide sequences.
				//
				//     kmer nucleotide 0 --> kmer binary bits 0, 1
				//     kmer nucleotide 1 --> kmer binary bits 2, 3
				//     ...
				//     kmer nucleotide 30 --> kmer binary bits 61, 62
				//
				// The SNP position splits the kmer into low_bits and high_bits.
				//
				// Assuming the SNP is at position "offset" in kmer, the kmer
				// nucleotides at positions offset, offset + 1, ...  would be encoded
				// by kmer binary bits at 2 * offset, 2 * offset + 1, ..., 62.  These
				// "high_bits" of the kmer form the LSBs of snp_repr[1].
				//
				// Conversely, the "low_bits" of the kmer binary representation, namely
				// bits 0, 1, ..., 2 * offset, 2 * offset + 1 form the MSBs of snp_repr[0].
				//
				// Note the nucleotide at the SNP position is redundantly represented,
				// being included in both the high_bits and the low_bits.  Thus,
				// the 2 LSBs of snp_repr[1] always equal the 2 MSBs of snp_repr[0].
				// This is intended as a partial correctnes check.
				//
				// The 2 LSBs of snp_repr[0] and the 2 MSBs of snp_repr[1] are unused,
				// and available for future extensions.
				//
				// As we construct snp_repr from kmers, we note which of the snp_repr bits
				// have been initialized so far, because future kmers for the snp must
				// agree with past kmers on those bits.  This is only a correctness check.
				// We do not persist these coverage masks to file.
				//
				// Finally, after we have constructed the optimized DB, we use it to reconstruct
				// from that the original DB, and compare to the original.  That is the final
				// and most definitive correctness self-check.
				//
				static_assert(sizeof(kmer) * 8 == 64);
				const auto low_bits = kmer << (62 - (offset * BITS_PER_BASE));
				const auto high_bits = kmer >> (offset * BITS_PER_BASE);
				assert(((low_bits >> 62) == (high_bits & 0x3)) && "SNP position differs in two supposedly redundant representations.");
				auto& snp_mask_0 = get<0>(snp_known_bits_mask);
				auto& snp_mask_1 = get<1>(snp_known_bits_mask);
				constexpr auto FULL_KMER = (LSB << K2) - LSB;
				// We've added information to the snp_repr.  Extend the coverage masks.
				const auto kmer_mask_0 = (FULL_KMER << (62 - (offset * BITS_PER_BASE)));
				const auto kmer_mask_1 = (FULL_KMER >> (offset * BITS_PER_BASE));
				const auto mask_0 = snp_mask_0 & kmer_mask_0;
				const auto mask_1 = snp_mask_1 & kmer_mask_1;
				if (snp_id == 5341 || ((mask_0 & get<0>(snp_repr)) != (mask_0 & low_bits))) {
					cerr << chrono_time() << ":  SNP " << snp_id << "(" << get<2>(snp_repr) << ")" << endl;
					cerr << chrono_time() << ":  most recent kmer 0x" << hex << kmer << dec << endl;
					cerr << chrono_time() << ":  " << endl;

					cerr << chrono_time() << ":  kmer[snp]: " << bitset<64>((LSB << offset * 2) | (LSB << (2 * offset + 1))) << endl;
					cerr << chrono_time() << ":  kmerbits:  " << bitset<64>(kmer) << endl;
					cerr << chrono_time() << ":  " << endl;

					cerr << chrono_time() << ":  lowbits:   " << bitset<64>(low_bits) << endl;
					cerr << chrono_time() << ":  kmer_mask0:" << bitset<64>(kmer_mask_0) << endl;
					cerr << chrono_time() << ":  mask0:     " << bitset<64>(snp_mask_0) << endl;
					cerr << chrono_time() << ":  snprepr0:  " << bitset<64>(get<0>(snp_repr)) << endl;
					cerr << chrono_time() << ":  " << endl;

					cerr << chrono_time() << ":  highbits:  " << bitset<64>(high_bits) << endl;
					cerr << chrono_time() << ":  kmer_mask1:" << bitset<64>(kmer_mask_1) << endl;
					cerr << chrono_time() << ":  mask1:     " << bitset<64>(snp_mask_1) << endl;
					cerr << chrono_time() << ":  snprepr1:  " << bitset<64>(get<1>(snp_repr)) << endl;
					if (((mask_0 & get<0>(snp_repr)) != (mask_0 & low_bits))) {
						cerr << "ERROR:  SNP above covered by conflicting kmers." << endl;
						assert(false);
					}
				}
				assert(((mask_1 & get<1>(snp_repr)) == (mask_1 & high_bits))  && "SNP covered by conflicting kmers");
				get<0>(snp_repr) |= low_bits;
				get<1>(snp_repr) |= high_bits;
				// We've added information to the snp_repr.  Extend the coverage masks.
				snp_mask_0 |= kmer_mask_0;
				snp_mask_1 |= kmer_mask_1;
			}
			if (end > 0 && lmer != last_lmer) {
				start = end / 2;
				++lmer_count;
			}
			// Invariant:  The data loaded so far for lmer reside at offsets start, start+1, ..., end-1.
			assert(start <= MAX_START);
			const auto len = (end / 2) - start + 1;
			assert(len < MAX_LEN);
			assert(lmer <= LMER_MASK);
			if (recompute_lmer_index) {
				lmer_index[lmer] = (start << LEN_BITS) | len;
			}
			last_lmer = lmer;
		}
		cerr << chrono_time() << ":  Validating optimized DB against original DB." << endl;
		assert(snps.size() == snps_map.size());
		for (uint64_t end = 0;  end < 20*1000*1000 /* db_filesize / 8 */;  end += 2) {
			const auto db_kmer = db_data[end + 1];
			const auto kmi = kmer_index[end / 2];
			const auto offset = kmi & 0x1f;
			const auto snp_id = kmi >> 5;
			assert(0 <= offset && offset < 31 && offset < K);
			assert(0 <= snp_id && snp_id <= snps.size());
			const auto& snp_repr = snps[snp_id];
			const auto low_bits = get<0>(snp_repr) >> (62 - (offset * BITS_PER_BASE));
			const auto high_bits = get<1>(snp_repr) << (offset * BITS_PER_BASE);
			assert(((get<0>(snp_repr) >> 62) == (get<1>(snp_repr) & 0x3)) && "SNP position differs in two supposedly redundant representations.");
			const auto kmer = (high_bits | low_bits) & BIT_MASK;
			assert(kmer == db_kmer);
		}
	}

	if (recompute_kmer_index_and_snps) {
		db_snps.save();
		db_kmer_index.save();
	}

	if (recompute_mmer_bloom) {
		db_mmer_bloom.save();
	}

	if (recompute_lmer_index) {
		db_lmer_index.save();
	}

	cerr << chrono_time() << ":  " << "Done with init for DB with " << (db_filesize / 16) << " kmers.  That took " << (chrono_time() - l_start) / 1000 << " seconds." << endl;

	l_start = chrono_time();

	vector<thread> th_array;
	int tmp_counter = 0;
	for(; optind < argc; optind++) {
		th_array.push_back(thread(kmer_lookup, lmer_index, mmer_bloom, db_kmer_index.address(), db_snps.address(), optind - in_pos, argv[optind], oname, M2, M3));
		++tmp_counter;

		if (tmp_counter >= n_threads) {

			cerr << chrono_time() << ":  " << "Waiting on all threads from this round to finish before dispatching next round." << endl;

			for (thread & ith : th_array) {
				ith.join();
				// cerr << chrono_time() << ":  " << " Thread joined " << endl;
			}

			cerr << chrono_time() << ":  " << "Ready to dispatch next round of threads." << endl;

			th_array.clear();
			tmp_counter = 0;
		}
	}

	if (th_array.size() > 0){
		for (thread & ith : th_array) {
			ith.join();
			// cerr << chrono_time() << ":  " << " Thread joined " << endl;
		}
		th_array.clear();
	}

	if (fd != -1 && db_data != NULL) {
		int rc = munmap(db_data, db_filesize);
		assert(rc == 0);
		close(fd);
	}

	cerr << chrono_time() << ":  " << " Totally done: " << (chrono_time() - l_start) / 1000 << " seconds elapsed processing reads, after DB was loaded."  << endl;

	return 0;
}
