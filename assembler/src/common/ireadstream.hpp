/*
 * ifastqstream.hpp
 *
 *  Created on: 03.03.2011
 *      Author: vyahhi
 */

#ifndef IREADSTREAM_HPP_
#define IREADSTREAM_HPP_

#include "libs/kseq/kseq.h"
#include <zlib.h>
#include <cassert>
#include "read.hpp"
#include "quality.hpp"
#include "nucl.hpp"

using namespace std;

// STEP 1: declare the type of file handler and the read() function
KSEQ_INIT(gzFile, gzread)

/*
 * Read name, seq and qual strings from FASTQ data (one by one)
 */
class ireadstream {

public:
	ireadstream(const string& filename) {
		filename_ = filename;
		is_open_ = open(filename);
		//assert(is_open_); // Fails if there is no such file -- don't do it!
	}

	virtual ~ireadstream() {
		close();
	}

	bool is_open() const {
		return is_open_;
	}

	bool eof() const {
		return eof_;
	}

	static vector<Read>* readAll(string filename, int cnt = -1) {
		ireadstream irs(filename);
		assert(irs.is_open());
		vector<Read>* res = new vector<Read>();
		Read r;
		while (cnt-- && irs.is_open() && !irs.eof()) {
			irs >> r;
			if (!r.isValid()) {
				cnt++;
				continue;
			}
			res->push_back(r);
		}
		irs.close();
		return res;
	}

	ireadstream& operator>>(Read &r) {
		if (!is_open() || eof()) {
			return *this;
		}

		// if there is 'N' in sequence, then throw out this mate read
		r.setName(seq_->name.s);
		r.setQuality(seq_->qual.s);
		r.setSequence(seq_->seq.s);
		/*for (size_t i = 0; i < seq_->seq.l; i++) { // Fix Ns to As so we can store ACGT in 2 bits (Sequence). Anyway we have a Quality values for filtering out Ns later
			if (!is_nucl(seq_->seq.s[i])) {
				seq_->seq.s[i] = 'A';
			}
		}
		r.setSequence(new Sequence(seq_->seq.s)); // it's create and copy :(*/

		read_ahead(); // make actual read for the next result
		return *this;
	}

	void close() {
		if (is_open()) {
			kseq_destroy(seq_); // STEP 5: destroy seq
			gzclose(fp_); // STEP 6: close the file handler
			is_open_ = false;
		}
	}

	void reset() {
		close();
		open(filename_);
	}

private:
	std::string filename_;
	gzFile fp_;
	kseq_t* seq_;
	bool is_open_;
	bool eof_;

	/*
	 * open i's file with FASTQ reads,
	 * return true if it opened file, false otherwise
	 */
	bool open(string filename) {
		fp_ = gzopen(filename.c_str(), "r"); // STEP 2: open the file handler
		if (!fp_) {
			return false;
		}
		is_open_ = true;
		seq_ = kseq_init(fp_); // STEP 3: initialize seq
		eof_ = false;
		read_ahead();
		return true;
	}

	void read_ahead() {
		assert(is_open());
		assert(!eof());
		if (kseq_read(seq_) < 0) {
			eof_ = true;
		}
	}
};

#endif /* IREADSTREAM_HPP_ */
