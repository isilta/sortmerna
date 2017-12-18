/**
 * @file paralleltraversal.cpp
 * @brief File containing functions for index traversal.
 * @parblock
 * SortMeRNA - next-generation reads filter for metatranscriptomic or total RNA
 * @copyright 2012-16 Bonsai Bioinformatics Research Group
 * @copyright 2014-16 Knight Lab, Department of Pediatrics, UCSD, La Jolla
 *
 * SortMeRNA is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SortMeRNA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with SortMeRNA. If not, see <http://www.gnu.org/licenses/>.
 * @endparblock
 *
 * @contributors Jenya Kopylova   jenya.kopylov@gmail.com
 *               Laurent Noé      laurent.noe@lifl.fr
 *               Pierre Pericard  pierre.pericard@lifl.fr
 *               Daniel McDonald  wasade@gmail.com
 *               Mikaël Salson    mikael.salson@lifl.fr
 *               Hélène Touzet    helene.touzet@lifl.fr
 *               Rob Knight       robknight@ucsd.edu
 */


#include <algorithm>
#include <locale>
#include <iomanip> // output formatting

#include "paralleltraversal.hpp"
#include "load_index.hpp"
#include "kseq.h"
#include "kseq_load.hpp"
#include "traverse_bursttrie.hpp"
#include "alignment.hpp"
#include "alignment2.hpp"
#include "mmap.hpp"

#include "options.hpp"
#include "ThreadPool.hpp"
#include "read.hpp"
#include "readstats.hpp"
#include "index.hpp"
#include "references.hpp"
#include "readsqueue.hpp"
#include "kvdb.hpp"
#include "processor.hpp"
#include "reader.hpp"
#include "writer.hpp"

#ifdef _OPENMP
#include <omp.h>
#endif

#if defined(_WIN32)
#define O_SMR_READ_BIN O_RDONLY | O_BINARY
#else
#define O_SMR_READ_BIN O_RDONLY
#endif

// forward
void writeLog(Runopts & opts, Index & index, Readstats & readstats, Output & output);

 // see "heuristic 1" below
 //#define HEURISTIC1_OFF

 /*! @brief Return complement of a nucleotide in
	 integer format.

	 <table>
	  <tr><th>i</th> <th>complement[i]</th></tr>
	  <tr><td>0 (A)</td> <td>3 (T)</td></tr>
	  <tr><td>1 (C)</td> <td>2 (G)</td></tr>
	  <tr><td>2 (G)</td> <td>1 (C)</td></tr>
	  <tr><td>3 (T)</td> <td>0 (A)</td></tr>
	 </table>
  */
char complement[4] = { 3,2,1,0 };

/* @function format_forward()
 * format the forward read into a string on same alphabet without '\n'
 *
 * */
void format_forward(char* read_seq, char* myread, char filesig)
{
	// FASTA
	if (filesig == '>')
	{
		while ((*read_seq != '\0') && (*read_seq != '>'))
		{
			if (*read_seq != '\n' || *read_seq != '\r') *myread++ = nt_table[(int)*read_seq];
			read_seq++;
		}
		*myread = '\n'; // end of read marked by newline
	}
	// FASTQ
	else
	{
		while (*read_seq != '\n' &&  *read_seq != '\r') { *myread++ = nt_table[(int)*read_seq++]; }
		*myread = '\n'; //end of read marked by newline
	}
}

/* @function format_rev()
 * format the reverse-complement read into a string without '\n'
 *
 * */
void format_rev(char* start_read, char* end_read, char* myread, char filesig)
{
	int8_t rc_table[128] = {
	  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	  4, 3, 4, 2, 4, 4, 4, 1, 4, 4, 4, 4, 4, 4, 4, 4,
	  4, 4, 4, 4, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	  4, 3, 4, 2, 4, 4, 4, 1, 4, 4, 4, 4, 4, 4, 4, 4,
	  4, 4, 4, 4, 0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4
	};

	// FASTA
	if (filesig == '>')
	{
		while (end_read != start_read)
		{
			if (*end_read != '\n' && *end_read != '\r') *myread++ = rc_table[(int)*end_read];
			end_read--;
		}
		*myread++ = rc_table[(int)*end_read];
		*myread = '\n';
	}
	// FASTQ
	else
	{
		while (*end_read != '\n' && *end_read != '\r') { *myread++ = rc_table[(int)*end_read--]; }
		*myread = '\n';
	}
}


// Callback run in a Processor thread
void parallelTraversalJob(Index & index, References & refs, Output & output, Readstats & readstats, Read & read)
{
	read.lastIndex = index.index_num;
	read.lastPart = index.part;

	// for reverse reads
	if (!index.opts.forward)
	{
		// output the first num_alignments_gv alignments
		if (num_alignments_gv > 0)
		{
			// all num_alignments_gv alignments have been output
			if (read.num_alignments < 0) return;
		}
		// the maximum scoring alignment has been found, go to next read
		// (unless all alignments are being output)
		else if (num_best_hits_gv > 0 && min_lis_gv > 0	&& read.max_SW_score == num_best_hits_gv)
			return;
	}

	bool read_to_count = true; // passed directly to compute_lis_alignment. TODO: What's the point?

	// find the minimum sequence length
	if (read.sequence.size() < readstats.min_read_len)
		readstats.min_read_len = static_cast<uint32_t>(read.sequence.size());
	// find the maximum sequence length
	if (read.sequence.size()  > readstats.max_read_len)
		readstats.max_read_len = static_cast<uint32_t>(read.sequence.size());

	// the read length is too short
	if (read.sequence.size()  < index.lnwin[index.index_num])
	{
		std::stringstream ss;
		ss << "\n  " << "\033[0;33m" << "WARNING" << "\033[0m"
			<< ": Processor thread: " << std::this_thread::get_id()
			<< " The read: " << read.id << " is shorter "
			<< "than " << index.lnwin[index.index_num] << " nucleotides, by default it will not be searched\n";
		std::cout << ss.str(); ss.str("");

		read.isValid = false;
		return;
	}

	uint32_t windowshift = index.opts.skiplengths[index.index_num][0];
	// keep track of windows which have been already traversed in the burst trie
	vector<bool> read_index_hits(read.sequence.size());

	uint32_t pass_n = 0; // Pass number (possible value 0,1,2)
	uint32_t max_SW_score = read.sequence.size() *index.opts.match; // the maximum SW score attainable for this read

	std::vector<MYBITSET> vbitwindowsf;
	std::vector<MYBITSET> vbitwindowsr;

	// TODO: these 2 lines need to be calculated once per index part. Move to index or some new calculation object
	uint32_t bit_vector_size = (index.partialwin[index.index_num] - 2) << 2; // on each index part. Init in Main, accessed in Worker
	uint32_t offset = (index.partialwin[index.index_num] - 3) << 2; // on each index part

	uint32_t minoccur = 0; // TODO: never updated. Always 0. What's the point?

	// loop for each new Pass to granulate seed search intervals
	for (bool search = true; search; )
	{
		uint32_t numwin = (read.sequence.size()
			- index.lnwin[index.index_num]
			+ windowshift) / windowshift; // number of k-mer windows fit along the sequence

		uint32_t win_index = 0; // index of the window's first char in the sequence e.g. 0, 18, 36 if window.length = 18
		// iterate over windows of the template string
		for (uint32_t win_num = 0; win_num < numwin; win_num++)
		{
			// skip position, seed at this position has already been searched for in a previous Pass
			if (read_index_hits[win_index]) goto check_score;
			// search position, set search bit to true
			else read_index_hits[win_index].flip();
			{
				// this flag it set to true if a match is found during
				// subsearch 1(a), to skip subsearch 1(b)
				bool accept_zero_kmer = false;
				// ids for k-mers that hit the database
				vector< id_win > id_hits; // TODO: why not to add directly to 'id_win_hits'?
				vbitwindowsf.resize(bit_vector_size);
				std::fill(vbitwindowsf.begin(), vbitwindowsf.end(), 0);

				init_win_f(&read.isequence[win_index + index.partialwin[index.index_num]],
					&vbitwindowsf[0],
					&vbitwindowsf[4],
					index.numbvs[index.index_num]);

				uint32_t keyf = 0;
				char *keyf_ptr = &read.isequence[win_index];
				// build hash for first half windows (foward and reverse)
				// hash is just a numeric value formed by the chars of a string consisting of '0','1','2','3'
				// e.g. "2233012" -> b10.1011.1100.0110 = x2BC6 = 11206
				for (uint32_t g = 0; g < index.partialwin[index.index_num]; g++)
				{
					(keyf <<= 2) |= (uint32_t)(*keyf_ptr);
					++keyf_ptr;
					//(keyf <<= 2) |= (uint32_t)*keyf_ptr++; // TODO: How did this work? And it did!
				}
				// do traversal if the exact half window exists in the burst trie
				if ((index.lookup_tbl[keyf].count > minoccur) && (index.lookup_tbl[keyf].trie_F != NULL))
				{
					/* subsearch (1)(a) d([p_1],[w_1]) = 0 and d([p_2],[w_2]) <= 1;
					*
					*  w = |------ [w_1] ------|------ [w_2] ------|
					*  p = |------ [p_1] ------|------ [p_2] ----| (0/1 deletion in [p_2])
					*              or
					*    = |------ [p_1] ------|------ [p_2] ------| (0/1 match/substitution in [p_2])
					*        or
					*    = |------ [p_1] ------|------ [p_2] --------| (0/1 insertion in [p_2])
					*
					*/
					traversetrie_align(
						index.lookup_tbl[keyf].trie_F,
						0,
						0,
						// win2f_k1_ptr
						&vbitwindowsf[0],
						// win2f_k1_full
						&vbitwindowsf[offset],
						accept_zero_kmer,
						id_hits,
						read.id,
						win_index,
						index.partialwin[index.index_num]
					);
				}//~if exact half window exists in the burst trie

					// only search if an exact match has not been found
				if (!accept_zero_kmer)
				{
					vbitwindowsr.resize(bit_vector_size);
					std::fill(vbitwindowsr.begin(), vbitwindowsr.end(), 0);

					// build the first bitvector window
					init_win_r(&read.isequence[win_index + index.partialwin[index.index_num] - 1],
						&vbitwindowsr[0],
						&vbitwindowsr[4],
						index.numbvs[index.index_num]);

					uint32_t keyr = 0;
					char *keyr_ptr = &read.isequence[win_index + index.partialwin[index.index_num]];

					// build hash for first half windows (foward and reverse)
					for (uint32_t g = 0; g < index.partialwin[index.index_num]; g++)
					{
						(keyr <<= 2) |= (uint32_t)(*keyr_ptr); //  - '0'
						++keyr_ptr;
					}

					// continue subsearch (1)(b)
					if ((index.lookup_tbl[keyr].count > minoccur) && (index.lookup_tbl[keyr].trie_R != NULL))
					{
						/* subsearch (1)(b) d([p_1],[w_1]) = 1 and d([p_2],[w_2]) = 0;
						*
						*  w =    |------ [w_1] ------|------ [w_2] -------|
						*  p =      |------- [p_1] ---|--------- [p_2] ----| (1 deletion in [p_1])
						*              or
						*    =    |------ [p_1] ------|------ [p_2] -------| (1 match/substitution in [p_1])
						*        or
						*    = |------- [p_1] --------|---- [p_2] ---------| (1 insertion in [p_1])
						*
						*/
						traversetrie_align(
							index.lookup_tbl[keyr].trie_R,
							0,
							0,
							&vbitwindowsr[0], /* win1r_k1_ptr */
							&vbitwindowsr[offset], /* win1r_k1_full */
							accept_zero_kmer,
							id_hits,
							read.id,
							win_index,
							index.partialwin[index.index_num]);
					}//~if exact half window exists in the reverse burst trie                    
				}//~if (!accept_zero_kmer)

					// associate the ids with the read window number
				if (!id_hits.empty())
				{
					for (uint32_t i = 0; i < id_hits.size(); i++)
					{
						read.id_win_hits.push_back(id_hits[i]);
					}
					read.readhit++;
				}
			}

		check_score:
			// continue read analysis if threshold seeds were matched
			if (win_num == numwin - 1)
			{
				compute_lis_alignment(
					read, index, refs, readstats, output,
					search, // returns False if the alignment is found -> stop searching
					max_SW_score,
					read_to_count
				);

				// the read was not accepted at current window skip length,
				// decrease the window skip length
				if (search)
				{
					// last (3rd) Pass has been made
					if (pass_n == 2) search = false;
					else
					{
						// the next interval size equals to the current one, skip it
						while ((pass_n < 3) &&
							(index.opts.skiplengths[index.index_num][pass_n] == index.opts.skiplengths[index.index_num][pass_n + 1])) ++pass_n;
						if (++pass_n > 2) search = false;
						// set interval skip length for next Pass
						else windowshift = index.opts.skiplengths[index.index_num][pass_n];
					}
				}
				break; // do not offset final window on read
			}//~( win_num == NUMWIN-1 )
			win_index += windowshift;
		}//~for (each window)                
			//~while all three window skip lengths have not been tested, or a match has not been found
	}// ~while (search);

	// the read didn't align (for --num_alignments [INT] option),
	// output null alignment string
	if (!read.hit && !index.opts.forward && (num_alignments_gv > -1))
	{
		// do not output read for de novo OTU clustering
		// (it did not pass the E-value threshold)
		if (de_novo_otu_gv && read.hit_denovo) read.hit_denovo = !read.hit_denovo; // flip
	}//~if read didn't align

	if (de_novo_otu_gv && read.hit_denovo)
		++readstats.total_reads_denovo_clustering;
} // ~parallelTraversalJob


void paralleltraversal(Runopts & opts)
{
	unsigned int numCores = std::thread::hardware_concurrency(); // find number of CPU cores
	std::cout << "CPU cores on this machine: " << numCores << std::endl; // 8

	// Init thread pool with the given number of threads
	int numThreads = 2 * opts.num_fread_threads + opts.num_proc_threads;
	if (numThreads > numCores)
		printf("WARN: Number of cores: %d is less than number allocated threads %d", numCores, numThreads);

	ThreadPool tpool(numThreads);
	KeyValueDatabase kvdb(opts.kvdbPath);
	ReadsQueue readQueue("read_queue", QUEUE_SIZE_MAX, 1); // shared: Processor pops, Reader pushes
	ReadsQueue writeQueue("write_queue", QUEUE_SIZE_MAX, opts.num_proc_threads); // shared: Processor pushes, Writer pops
	Readstats readstats(opts);
	Output output(opts, readstats);
	Index index(opts, readstats, output);
	References refs(opts, index);

	int loopCount = 0; // counter of total number of processing iterations

	// perform alignment
	// loop through every index passed to option --ref (ex. SSU 16S and SSU 18S)
	for (uint16_t index_num = 0; index_num < (uint16_t)opts.indexfiles.size(); ++index_num)
	{
		// iterate every part of an index
		for (uint16_t idx_part = 0; idx_part < index.num_index_parts[index_num]; ++idx_part)
		{
			eprintf("\tLoading index part %d/%u ... ", idx_part + 1, index.num_index_parts[index_num]);
			auto t = std::chrono::high_resolution_clock::now();
			index.load(index_num, idx_part);
			refs.load(index_num, idx_part);
			std::chrono::duration<double> elapsed = std::chrono::high_resolution_clock::now() - t; // ~20 sec Debug/Win
//			std::chrono::duration<double> elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::high_resolution_clock::now() - t);
			eprintf("done [%.2f sec]\n", elapsed.count());

			for (int i = 0; i < opts.num_fread_threads; i++)
			{
				tpool.addJob(Reader("reader_" + std::to_string(i), opts, readQueue, kvdb, loopCount));
				tpool.addJob(Writer("writer_" + std::to_string(i), writeQueue, kvdb));
			}

			// add processor jobs
			for (int i = 0; i < opts.num_proc_threads; i++)
			{
				tpool.addJob(Processor("proc_" + std::to_string(i), readQueue, writeQueue, readstats, index, refs, output, parallelTraversalJob));
			}
			++loopCount;
		} // ~for(idx_part)
	} // ~for(index_num)

	tpool.waitAll(); // wait till processing is done

	writeLog(opts, index, readstats, output);
} // ~paralleltraversal2

void writeLog(Runopts & opts, Index & index, Readstats & readstats, Output & output)
{
	//FILE* bilan = fopen(output.logoutfile, "ab"); // output::logoutfile
	output.logout.open(output.logoutfile, std::ofstream::binary | std::ofstream::app);

	// output total number of reads
	output.logout << " Results:\n";
	output.logout << "    Total reads = " << readstats.number_total_read << "\n";
	//fprintf(bilan, " Results:\n");
	//fprintf(bilan, "    Total reads = %llu\n", readstats.number_total_read); // Readstats::number_total_read
	if (de_novo_otu_gv)
	{
		// total_reads_denovo_clustering = sum of all reads that have read::hit_denovo == true
		// either query DB or store in Readstats::total_reads_denovo_clustering
		output.logout << "    Total reads for de novo clustering = " << readstats.total_reads_denovo_clustering << "\n";
		//fprintf(bilan, "    Total reads for de novo clustering = %llu\n", total_reads_denovo_clustering);
	}
	// output total non-rrna + rrna reads
	output.logout << std::setprecision(2) << std::fixed;
	output.logout << "    Total reads passing E-value threshold = " << readstats.total_reads_mapped 
		<< " (" << (float)((float)readstats.total_reads_mapped / (float)readstats.number_total_read) * 100 << ")\n";
	output.logout << "    Total reads failing E-value threshold = "
		<< readstats.number_total_read - readstats.total_reads_mapped
		<< " ("	<< (1 - ((float)((float)readstats.total_reads_mapped / (float)readstats.number_total_read))) * 100 << ")\n";
	output.logout << "    Minimum read length = " << readstats.min_read_len << "\n";
	output.logout << "    Maximum read length = " << readstats.max_read_len << "\n";
	output.logout << "    Mean read length    = " << readstats.full_read_main / readstats.number_total_read << "\n";

	output.logout << " By database:\n";
	//fprintf(bilan, "    Total reads passing E-value threshold = %llu (%.2f%%)\n", 
	//	readstats.total_reads_mapped, (float)((float)readstats.total_reads_mapped / (float)readstats.number_total_read) * 100);
	//fprintf(bilan, "    Total reads failing E-value threshold = %llu (%.2f%%)\n", 
	//	readstats.number_total_read - readstats.total_reads_mapped, 
	//	(1 - ((float)((float)readstats.total_reads_mapped / (float)readstats.number_total_read))) * 100);
	//fprintf(bilan, "    Minimum read length = %u\n", readstats.min_read_len);
	//fprintf(bilan, "    Maximum read length = %u\n", readstats.max_read_len);
	//fprintf(bilan, "    Mean read length = %u\n", readstats.full_read_main / readstats.number_total_read);
	//fprintf(bilan, " By database:\n");
	// output stats by database
	for (uint32_t index_num = 0; index_num < opts.indexfiles.size(); index_num++)
	{
		output.logout << "    " << opts.indexfiles[index_num].first << "\t\t"
			<< (float)((float)readstats.reads_matched_per_db[index_num] / (float)readstats.number_total_read) * 100 << "\n";
		//fprintf(bilan, "    %s\t\t%.2f%%\n", (char*)(opts.indexfiles[index_num].first).c_str(), 
		//	(float)((float)reads_matched_per_db[index_num] / (float)readstats.number_total_read) * 100);
	}

	if (otumapout_gv)
	{
		output.logout << " Total reads passing %%id and %%coverage thresholds = " << readstats.total_reads_mapped_cov << "\n";
		output.logout << " Total OTUs = " << readstats.otu_total << "\n"; // otu_map.size()
		//fprintf(bilan, " Total reads passing %%id and %%coverage thresholds = %llu\n", total_reads_mapped_cov);
		//fprintf(bilan, " Total OTUs = %lu\n", otu_map.size());
	}
	time_t q = time(0);
	struct tm * now = localtime(&q);
	output.logout << "\n " << asctime(now) << "\n";
	output.logout.close();
} // ~writeLog
