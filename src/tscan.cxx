/*
  T-scan

  Copyright (c) 1998 - 2015

  This file is part of tscan

  tscan is free software; you can redistribute it and/or modify
  it under the terms of the GNU Affero General Public License as published by
  the Free Software Foundation; either version 3 of the License, or
  (at your option) any later version.

  tscan is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Affere General Public License for more details.

  You should have received a copy of the GNU Affero General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.

*/

#include <string>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <sys/types.h>
#include <sys/stat.h>
#include "config.h"
#ifdef HAVE_OPENMP
#include "omp.h"
#endif

#include "ticcutils/FdStream.h"
#include "ticcutils/ServerBase.h"
#include "ticcutils/StringOps.h"
#include "ticcutils/Configuration.h"
#include "ticcutils/FileUtils.h"
#include "ticcutils/CommandLine.h"
#include "ticcutils/XMLtools.h"
#include "libfolia/folia.h"
#include "frog/FrogAPI.h"
#include "tscan/Alpino.h"
#include "tscan/cgn.h"
#include "tscan/sem.h"
#include "tscan/intensify.h"
#include "tscan/conn.h"
#include "tscan/general.h"
#include "tscan/situation.h"
#include "tscan/afk.h"
#include "tscan/adverb.h"
#include "tscan/ner.h"
#include "tscan/utils.h"
#include "tscan/stats.h"

using namespace std;
using namespace TiCC;

const string frog_pos_set = "http://ilk.uvt.nl/folia/sets/frog-mbpos-cgn";
const string frog_lemma_set = "http://ilk.uvt.nl/folia/sets/frog-mblem-nl";
const string frog_morph_set = "http://ilk.uvt.nl/folia/sets/frog-mbma-nl";

string configFile = "tscan.cfg";
string probFilename = "problems.log";
ofstream problemFile;
TiCC::Configuration config;
string workdir_name;

struct cf_data {
  long int count;
  double freq;
};

struct noun {
  SEM::Type type;
  bool is_compound;
  string head;
  string satellite;
  string satellite_clean;
  int compound_parts;
};

struct settingData {
  void init( const TiCC::Configuration& );
  bool doAlpino;
  bool doAlpinoServer;
  bool doWopr;
  bool doLsa;
  bool doXfiles;
  bool showProblems;
  bool sentencePerLine;
  string style;
  int rarityLevel;
  unsigned int overlapSize;
  double freq_clip;
  double mtld_threshold;
  map<string, SEM::Type> adj_sem;
  map<string, noun> noun_sem;
  map<string, SEM::Type> verb_sem;
  map<string, Intensify::Type> intensify;
  map<string, General::Type> general_nouns;
  map<string, General::Type> general_verbs;
  map<string, Adverb::Type> adverbs;
  map<string, double> pol_lex;
  map<string, cf_data> staph_word_freq_lex;
  long int staph_total;
  map<string, cf_data> word_freq_lex;
  long int word_total;
  map<string, cf_data> lemma_freq_lex;
  long int lemma_total;
  map<string, top_val> top_freq_lex;
  map<CGN::Type, set<string> > temporals1;
  set<string> multi_temporals;
  map<CGN::Type, set<string> > causals1;
  set<string> multi_causals;
  map<CGN::Type, set<string> > opsommers_wg;
  set<string> multi_opsommers_wg;
  map<CGN::Type, set<string> > opsommers_zin;
  set<string> multi_opsommers_zin;
  map<CGN::Type, set<string> > contrast1;
  set<string> multi_contrast;
  map<CGN::Type, set<string> > compars1;
  set<string> multi_compars;
  map<CGN::Type, set<string> > causal_sits;
  set<string> multi_causal_sits;
  map<CGN::Type, set<string> > space_sits;
  set<string> multi_space_sits;
  map<CGN::Type, set<string> > time_sits;
  set<string> multi_time_sits;
  map<CGN::Type, set<string> > emotion_sits;
  set<string> multi_emotion_sits;
  set<string> vzexpr2;
  set<string> vzexpr3;
  set<string> vzexpr4;
  map<string,Afk::Type> afkos;
};

settingData settings;

bool fillN( map<string,noun>& m, istream& is ){
  string line;
  while( safe_getline( is, line ) ){
    // Trim the lines
    line = TiCC::trim( line );
    if ( line.empty() )
      continue;

    // Split at a tab; the line should contain either 3 (non-compounds) or 7 (compounds) values
    vector<string> parts;
    int i = TiCC::split_at( line, parts, "\t" );
    if (i != 3 && i != 7) {
      cerr << "skip line: " << line << " (expected 3 or 7 values, got " << i << ")" << endl;
      continue;
    }

    // Classify the noun, set the compound values and add the noun to the map
    noun n;
    n.type = SEM::classifyNoun(parts[1]);
    n.is_compound = parts[2] == "1";
    if (n.is_compound) {
      n.head = parts[3];
      n.satellite = parts[4];
      n.satellite_clean = parts[5];
      n.compound_parts = atoi(parts[6].c_str());
    }
    m[parts[0]] = n;
  }
  return true;
}

bool fillN( map<string,noun>& m, const string& filename ) {
  ifstream is( filename.c_str() );
  if (is) {
    return fillN(m, is);
  }
  else {
    cerr << "couldn't open file: " << filename << endl;
  }
  return false;
}

bool fillWW( map<string,SEM::Type>& m, istream& is ){
  string line;
  while( safe_getline( is, line ) ){
    line = TiCC::trim( line );
    if ( line.empty() )
      continue;
    vector<string> parts;
    int n = TiCC::split_at( line, parts, "\t" ); // split at tab
    if ( n != 3 ){
      cerr << "skip line: " << line << " (expected 3 values, got "
	   << n << ")" << endl;
      continue;
    }
    SEM::Type res = SEM::classifyWW( parts[1], parts[2] );
    if ( res != SEM::UNFOUND_VERB ){
      // no use to store undefined values
      m[parts[0]] = res;
    }
  }
  return true;
}

bool fillADJ( map<string,SEM::Type>& m, istream& is ){
  string line;
  while( safe_getline( is, line ) ){
    line = TiCC::trim( line );
    if ( line.empty() )
      continue;
    vector<string> parts;
    int n = TiCC::split_at( line, parts, "\t" ); // split at tab
    if ( n <2 || n > 3 ){
      cerr << "skip line: " << line << " (expected 2 or 3 values, got "
	   << n << ")" << endl;
      continue;
    }
    SEM::Type res = SEM::UNFOUND_ADJ;
    if ( n == 2 ){
      res = SEM::classifyADJ( parts[1] );
    }
    else {
      res = SEM::classifyADJ( parts[1], parts[2] );
    }
    string low = TiCC::lowercase( parts[0] );
    if ( m.find(low) != m.end() ){
      cerr << "Information: multiple entry '" << low << "' in ADJ lex" << endl;
    }
    if ( res != SEM::UNFOUND_ADJ ){
      // no use to store undefined values
      m[low] = res;
    }
  }
  return true;
}

bool fill( CGN::Type tag, map<string,SEM::Type>& m, const string& filename ){
  ifstream is( filename.c_str() );
  if ( is ){
    if ( tag == CGN::WW )
      return fillWW( m, is );
    else if ( tag == CGN::ADJ )
      return fillADJ( m, is );
  }
  else {
    cerr << "couldn't open file: " << filename << endl;
  }
  return false;
}

bool fill_intensify(map<string,Intensify::Type>& m, istream& is){
  string line;
  while( safe_getline( is, line ) ){
    line = TiCC::trim( line );
    if ( line.empty() )
      continue;
    vector<string> parts;
    int n = TiCC::split_at( line, parts, "\t" ); // split at tab
    if ( n < 2 || n > 2 ){
      cerr << "skip line: " << line << " (expected 2 values, got "
       << n << ")" << endl;
      continue;
    }
    string low = TiCC::trim(TiCC::lowercase( parts[0] ));
    Intensify::Type res = Intensify::classify(TiCC::lowercase(parts[1]));
    if ( m.find(low) != m.end() ){
      cerr << "Information: multiple entry '" << low << "' in Intensify lex" << endl;
    }
    if ( res != Intensify::NO_INTENSIFY ){
      // no use to store undefined values
      m[low] = res;
    }
  }
  return true;
}

bool fill_intensify(map<string,Intensify::Type>& m, const string& filename) {
  ifstream is( filename.c_str() );
  if (is) {
    return fill_intensify(m, is);
  }
  else {
    cerr << "couldn't open file: " << filename << endl;
  }
  return false;
}

bool fill_general(map<string,General::Type>& m, istream& is){
  string line;
  while( safe_getline( is, line ) ){
    line = TiCC::trim( line );
    if ( line.empty() )
      continue;
    vector<string> parts;
    int n = TiCC::split_at( line, parts, "\t" ); // split at tab
    if ( n < 2 || n > 2 ){
      cerr << "skip line: " << line << " (expected 2 values, got "
       << n << ")" << endl;
      continue;
    }
    string low = TiCC::trim(TiCC::lowercase( parts[0] ));
    General::Type res = General::classify(TiCC::lowercase(parts[1]));
    if ( m.find(low) != m.end() ){
      cerr << "Information: multiple entry '" << low << "' in general lex" << endl;
    }
    if ( res != General::NO_GENERAL ){
      // no use to store undefined values
      m[low] = res;
    }
  }
  return true;
}

bool fill_general(map<string,General::Type>& m, const string& filename) {
  ifstream is( filename.c_str() );
  if (is) {
    return fill_general(m, is);
  }
  else {
    cerr << "couldn't open file: " << filename << endl;
  }
  return false;
}

bool fill_adverbs(map<string,Adverb::Type>& m, istream& is){
  string line;
  while( safe_getline( is, line ) ){
    line = TiCC::trim( line );
    if ( line.empty() )
      continue;
    vector<string> parts;
    int n = TiCC::split_at( line, parts, "\t" ); // split at tab
    if ( n < 2 || n > 2 ){
      cerr << "skip line: " << line << " (expected 2 values, got "
       << n << ")" << endl;
      continue;
    }
    string low = TiCC::trim(TiCC::lowercase(parts[0]));
    Adverb::Type res = Adverb::classify(TiCC::lowercase(parts[1]));
    if ( m.find(low) != m.end() ){
      cerr << "Information: multiple entry '" << low << "' in adverbs lex" << endl;
    }
    if ( res != Adverb::NO_ADVERB ){
      // no use to store undefined values
      m[low] = res;
    }
  }
  return true;
}

bool fill_adverbs(map<string,Adverb::Type>& m, const string& filename) {
  ifstream is( filename.c_str() );
  if (is) {
    return fill_adverbs(m, is);
  }
  else {
    cerr << "couldn't open file: " << filename << endl;
  }
  return false;
}

bool fill_freqlex( map<string,cf_data>& m, long int& total, istream& is ){
  total = 0;
  string line;
  while( safe_getline( is, line ) ){
    line = TiCC::trim( line );
    if ( line.empty() )
      continue;
    vector<string> parts;
    size_t n = TiCC::split_at( line, parts, "\t" ); // split at tabs
    if ( n != 4 ){
      cerr << "skip line: " << line << " (expected 4 values, got "
	   << n << ")" << endl;
      continue;
    }
    cf_data data;
    data.count = TiCC::stringTo<long int>( parts[1] );
    data.freq = TiCC::stringTo<double>( parts[3] );
    if ( data.count == 1 ){
      // we are done. Skip all singleton stuff
      return true;
    }
    if ( settings.freq_clip > 0 ){
      // skip low frequent word, when desired
      if ( data.freq > settings.freq_clip ){
	return true;
      }
    }
    total += data.count;
    m[parts[0]] = data;
  }
  return true;
}

bool fill_freqlex( map<string,cf_data>& m, long int& total,
		   const string& filename ){
  ifstream is( filename.c_str() );
  if ( is ){
    fill_freqlex( m, total, is );
    cout << "read " << filename << " (" << total << " entries)" << endl;
    return true;
  }
  else {
    cerr << "couldn't open file: " << filename << endl;
  }
  return false;
}

bool fill_topvals( map<string,top_val>& m, istream& is ){
  string line;
  int line_count = 0;
  top_val val = top2000;
  while( safe_getline( is, line ) ){
    line = TiCC::trim( line );
    if ( line.empty() )
      continue;
    ++line_count;
    if ( line_count > 10000 )
      val = top20000;
    else if ( line_count > 5000 )
      val = top10000;
    else if ( line_count > 3000 )
      val = top5000;
    else if ( line_count > 2000 )
      val = top3000;
    else if ( line_count > 1000 )
      val = top2000;
    else
      val = top1000;
    vector<string> parts;
    size_t n = TiCC::split_at( line, parts, "\t" ); // split at tabs
    if ( n != 4 ){
      cerr << "skip line: " << line << " (expected 2 values, got "
	   << n << ")" << endl;
      continue;
    }
    m[parts[0]] = val;
  }
  return true;
}

bool fill_topvals( map<string,top_val>& m, const string& filename ){
  ifstream is( filename.c_str() );
  if ( is ){
    return fill_topvals( m, is );
  }
  else {
    cerr << "couldn't open file: " << filename << endl;
  }
  return false;
}

bool fill_connectors( map<CGN::Type,set<string> >& c1,
		      set<string>& cM,
		      istream& is ){
  cM.clear();
  string line;
  while( safe_getline( is, line ) ){
    // a line is supposed to be :
    // a comment, starting with '#'
    // like: '# comment'
    // OR an entry of 1 to 4 words seperated by a single space
    // like: 'dus' OR 'de facto'
    // OR the 1 word followed by a TAB ('\t') and a CGN tag
    // like: 'maar   VG'
    line = TiCC::trim( line );
    if ( line.empty() || line[0] == '#' )
      continue;
    vector<string> vec;
    int n = TiCC::split_at( line, vec, "\t" );
    if ( n == 0 || n > 2 ){
      cerr << "skip line: " << line << " (expected 1 or 2 values, got "
	   << n << ")" << endl;
      continue;
    }
    CGN::Type tag = CGN::UNASS;
    if ( n == 2 ){
      tag = CGN::toCGN( vec[1] );
    }
    vector<string> dum;
    n = TiCC::split_at( vec[0], dum, " " );
    if ( n < 1 || n > 4 ){
      cerr << "skip line: " << line
	   << " (expected 1, to 4 values in the first part: " << vec[0]
	   << ", got " << n << ")" << endl;
      continue;
    }
    if ( n == 1 ){
      c1[tag].insert( vec[0] );
    }
    else if ( n > 1 && tag != CGN::UNASS ){
      cerr << "skip line: " << line
	   << " (no GCN tag info allowed for multiword entries) " << endl;
      continue;
    }
    else {
      cM.insert( vec[0] );
    }
  }
  return true;
}

bool fill_connectors( map<CGN::Type, set<string> >& c1,
		      set<string>& cM,
		      const string& filename ){
  ifstream is( filename.c_str() );
  if ( is ){
    return fill_connectors( c1, cM, is );
  }
  else {
    cerr << "couldn't open file: " << filename << endl;
  }
  return false;
}

bool fill_vzexpr( set<string>& vz2, set<string>& vz3, set<string>& vz4,
		  istream& is ){
  string line;
  while( safe_getline( is, line ) ){
    // a line is supposed to be :
    // a comment, starting with '#'
    // like: '# comment'
    // OR an entry of 2, 3 or 4 words seperated by whitespace
    line = TiCC::trim( line );
    if ( line.empty() || line[0] == '#' )
      continue;
    vector<string> vec;
    int n = TiCC::split_at_first_of( line, vec, " \t" );
    if ( n == 0 || n > 4 ){
      cerr << "skip line: " << line << " (expected 2, 3 or 4 values, got "
	   << n << ")" << endl;
      continue;
    }
    switch ( n ){
    case 2: {
      string line = vec[0] + " " + vec[1];
      vz2.insert( line );
    }
      break;
    case 3: {
      string line = vec[0] + " " + vec[1] + " " + vec[2];
      vz3.insert( line );
    }
      break;
    case 4: {
      string line = vec[0] + " " + vec[1] + " " + vec[2] + " " + vec[3];
      vz4.insert( line );
    }
      break;
    default:
      throw logic_error( "switch out of range" );
    }
  }
  return true;
}

bool fill_vzexpr( set<string>& vz2, set<string>& vz3, set<string>& vz4,
		  const string& filename ){
  ifstream is( filename.c_str() );
  if ( is ){
    return fill_vzexpr( vz2, vz3, vz4 , is );
  }
  else {
    cerr << "couldn't open file: " << filename << endl;
  }
  return false;
}

bool fill( map<string,Afk::Type>& afkos, istream& is ){
  string line;
  while( safe_getline( is, line ) ){
    // a line is supposed to be :
    // a comment, starting with '#'
    // like: '# comment'
    // OR an entry of 2 words seperated by whitespace
    line = TiCC::trim( line );
    if ( line.empty() || line[0] == '#' )
      continue;
    vector<string> vec;
    int n = TiCC::split_at_first_of( line, vec, " \t" );
    if ( n < 2 ){
      cerr << "skip line: " << line << " (expected at least 2 values, got "
	   << n << ")" << endl;
      continue;
    }
    if ( n == 2 ){
      Afk::Type at = Afk::classify( vec[1] );
      if ( at != Afk::NO_A )
	afkos[vec[0]] = at;
    }
    else if ( n == 3 ){
      Afk::Type at = Afk::classify( vec[2] );
      if ( at != Afk::NO_A ){
	string s = vec[0] + " " + vec[1];
	afkos[s] = at;
      }
    }
    else if ( n == 4 ){
      Afk::Type at = Afk::classify( vec[3] );
      if ( at != Afk::NO_A ){
	string s = vec[0] + " " + vec[1] + " " + vec[2];
	afkos[s] = at;
      }
    }
    else {
      cerr << "skip line: " << line << " (expected at most 4 values, got "
	   << n << ")" << endl;
      continue;
    }
  }
  return true;
}

bool fill( map<string,Afk::Type>& afks, const string& filename ){
  ifstream is( filename.c_str() );
  if ( is ){
    return fill( afks , is );
  }
  else {
    cerr << "couldn't open file: " << filename << endl;
  }
  return false;
}

void settingData::init( const Configuration& cf ){
  doXfiles = true;
  doAlpino = false;
  doAlpinoServer = false;
  string val = cf.lookUp( "useAlpinoServer" );
  if ( !val.empty() ){
    if ( !TiCC::stringTo( val, doAlpinoServer ) ){
      cerr << "invalid value for 'useAlpinoServer' in config file" << endl;
      exit( EXIT_FAILURE );
    }
  }
  if ( !doAlpinoServer ){
    val = cf.lookUp( "useAlpino" );
    if( !TiCC::stringTo( val, doAlpino ) ){
      cerr << "invalid value for 'useAlpino' in config file" << endl;
      exit( EXIT_FAILURE );
    }
  }
  doWopr = false;
  val = cf.lookUp( "useWopr" );
  if ( !val.empty() ){
    if ( !TiCC::stringTo( val, doWopr ) ){
      cerr << "invalid value for 'useWopr' in config file" << endl;
      exit( EXIT_FAILURE );
    }
  }
  doLsa = false;
  val = cf.lookUp( "useLsa" );
  if ( !val.empty() ){
    if ( !TiCC::stringTo( val, doLsa ) ){
      cerr << "invalid value for 'useLsa' in config file" << endl;
      exit( EXIT_FAILURE );
    }
    if ( doLsa ){
      cerr << "sorry, but LSA is disabled. Please remove 'useLsa' from the "
     	   << "config file, or set it to false." << endl;
      exit( EXIT_FAILURE );
    }
  }
  showProblems = true;
  val = cf.lookUp( "logProblems" );
  if ( !val.empty() ){
    if ( !TiCC::stringTo( val, showProblems ) ){
      cerr << "invalid value for 'showProblems' in config file" << endl;
      exit( EXIT_FAILURE );
    }
  }
  sentencePerLine = false;
  val = cf.lookUp( "sentencePerLine" );
  if ( !val.empty() ){
    if ( !TiCC::stringTo( val, sentencePerLine ) ){
      cerr << "invalid value for 'sentencePerLine' in config file" << endl;
      exit( EXIT_FAILURE );
    }
  }
  val = cf.lookUp( "styleSheet" );
  if( !val.empty() ){
    style = val;
  }
  val = cf.lookUp( "rarityLevel" );
  if ( val.empty() ){
    rarityLevel = 10;
  }
  else if ( !TiCC::stringTo( val, rarityLevel ) ){
    cerr << "invalid value for 'rarityLevel' in config file" << endl;
  }
  val = cf.lookUp( "overlapSize" );
  if ( val.empty() ){
    overlapSize = 50;
  }
  else if ( !TiCC::stringTo( val, overlapSize ) ){
    cerr << "invalid value for 'overlapSize' in config file" << endl;
    exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "frequencyClip" );
  if ( val.empty() ){
    freq_clip = 90;
  }
  else if ( !TiCC::stringTo( val, freq_clip )
	    || (freq_clip < 0) || (freq_clip > 100) ){
    cerr << "invalid value for 'frequencyClip' in config file" << endl;
    exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "mtldThreshold" );
  if ( val.empty() ){
    mtld_threshold = 0.720;
  }
  else if ( !TiCC::stringTo( val, mtld_threshold )
	    || (mtld_threshold < 0) || (mtld_threshold > 1.0) ){
    cerr << "invalid value for 'frequencyClip' in config file" << endl;
    exit( EXIT_FAILURE );
  }

  val = cf.lookUp( "adj_semtypes" );
  if ( !val.empty() ){
    if ( !fill( CGN::ADJ, adj_sem, val ) ) // 20150316: Full path necessary to allow custom input
      exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "noun_semtypes" );
  if ( !val.empty() ){
    if ( !fillN( noun_sem, val ) ) // 20141121: Full path necessary to allow custom input
      exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "verb_semtypes" );
  if ( !val.empty() ){
    if ( !fill( CGN::WW, verb_sem, cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "intensify" );
  if ( !val.empty() ){
    if ( !fill_intensify( intensify, val ) )
      exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "general_nouns" );
  if ( !val.empty() ){
    if ( !fill_general( general_nouns, cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "general_verbs" );
  if ( !val.empty() ){
    if ( !fill_general( general_verbs, cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "adverbs" );
  if ( !val.empty() ){
    if ( !fill_adverbs( adverbs, cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }
  staph_total = 0;
  val = cf.lookUp( "staph_word_freq_lex" );
  if ( !val.empty() ){
    if ( !fill_freqlex( staph_word_freq_lex, staph_total,
			cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }
  word_total = 0;
  val = cf.lookUp( "word_freq_lex" );
  if ( !val.empty() ){
    if ( !fill_freqlex( word_freq_lex, word_total,
			cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }
  lemma_total = 0;
  val = cf.lookUp( "lemma_freq_lex" );
  if ( !val.empty() ){
    if ( !fill_freqlex( lemma_freq_lex, lemma_total,
			cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "top_freq_lex" );
  if ( !val.empty() ){
    if ( !fill_topvals( top_freq_lex, cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "temporals" );
  if ( !val.empty() ){
    if ( !fill_connectors( temporals1, multi_temporals, cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "opsom_connectors_wg" );
  if ( !val.empty() ){
    if ( !fill_connectors( opsommers_wg, multi_opsommers_wg, cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "opsom_connectors_zin" );
  if ( !val.empty() ){
    if ( !fill_connectors( opsommers_zin, multi_opsommers_zin, cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "contrast" );
  if ( !val.empty() ){
    if ( !fill_connectors( contrast1, multi_contrast, cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "compars" );
  if ( !val.empty() ){
    if ( !fill_connectors( compars1, multi_compars, cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "causals" );
  if ( !val.empty() ){
    if ( !fill_connectors( causals1, multi_causals, cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "causal_situation" );
  if ( !val.empty() ){
    if ( !fill_connectors( causal_sits, multi_causal_sits, cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "space_situation" );
  if ( !val.empty() ){
    if ( !fill_connectors( space_sits, multi_space_sits, cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "time_situation" );
  if ( !val.empty() ){
    if ( !fill_connectors( time_sits, multi_time_sits, cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }

  val = cf.lookUp( "emotion_situation" );
  if ( !val.empty() ){
    if ( !fill_connectors( emotion_sits, multi_emotion_sits, cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }

  val = cf.lookUp( "voorzetselexpr" );
  if ( !val.empty() ){
    if ( !fill_vzexpr( vzexpr2, vzexpr3, vzexpr4, cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }
  val = cf.lookUp( "afkortingen" );
  if ( !val.empty() ){
    if ( !fill( afkos, cf.configDir() + "/" + val ) )
      exit( EXIT_FAILURE );
  }
}

inline void usage(){
  cerr << "usage:  tscan [options] <inputfiles> " << endl;
  cerr << "options: " << endl;
  cerr << "\t-o <file> store XML in 'file' " << endl;
  cerr << "\t--config=<file> read configuration from 'file' " << endl;
  cerr << "\t-V or --version show version " << endl;
  cerr << "\t-n assume input file to hold one sentence per line" << endl;
  cerr << "\t--skip=[aclw]    Skip Alpino (a), CSV output (c), Lsa (l) or Wopr (w).\n";
  cerr << "\t-t <file> process the 'file'. (deprecated)" << endl;
  cerr << endl;
}

template <class M>
void aggregate( M& out, const M& in ){
  typename M::const_iterator ii = in.begin();
  while ( ii != in.end() ){
    typename M::iterator oi = out.find( ii->first );
    if ( oi == out.end() ){
      out[ii->first] = ii->second;
    }
    else {
      oi->second += ii->second;
    }
    ++ii;
  }
}

void aggregate( multimap<DD_type,int>& out,
		const multimap<DD_type,int>& in ){
  multimap<DD_type,int>::const_iterator ii = in.begin();
  while ( ii != in.end() ){
    out.insert( make_pair(ii->first, ii->second ) );
    ++ii;
  }
}

Conn::Type wordStats::checkConnective() const {
  if ( tag != CGN::VG && tag != CGN::VZ && tag != CGN::BW )
    return Conn::NOCONN;

  if ( settings.temporals1[tag].find( l_word )
       != settings.temporals1[tag].end() )
    return Conn::TEMPOREEL;
  else if ( settings.temporals1[CGN::UNASS].find( l_word )
	    != settings.temporals1[CGN::UNASS].end() )
    return Conn::TEMPOREEL;

  else if ( settings.opsommers_wg[tag].find( l_word )
      != settings.opsommers_wg[tag].end() )
    return Conn::OPSOMMEND_WG;
  else if ( settings.opsommers_wg[CGN::UNASS].find( l_word )
      != settings.opsommers_wg[CGN::UNASS].end() )
    return Conn::OPSOMMEND_WG;

  else if ( settings.opsommers_zin[tag].find( l_word )
      != settings.opsommers_zin[tag].end() )
    return Conn::OPSOMMEND_ZIN;
  else if ( settings.opsommers_zin[CGN::UNASS].find( l_word )
      != settings.opsommers_zin[CGN::UNASS].end() )
    return Conn::OPSOMMEND_ZIN;

  else if ( settings.contrast1[tag].find( l_word )
	    != settings.contrast1[tag].end() )
    return Conn::CONTRASTIEF;
  else if ( settings.contrast1[CGN::UNASS].find( l_word )
	    != settings.contrast1[CGN::UNASS].end() )
    return Conn::CONTRASTIEF;

  else if ( settings.compars1[tag].find( l_word )
	    != settings.compars1[tag].end() )
    return Conn::COMPARATIEF;
  else if ( settings.compars1[CGN::UNASS].find( l_word )
	    != settings.compars1[CGN::UNASS].end() )
    return Conn::COMPARATIEF;

  else if ( settings.causals1[tag].find( l_word )
	    != settings.causals1[tag].end() )
    return Conn::CAUSAAL;
  else if ( settings.causals1[CGN::UNASS].find( l_word )
	    != settings.causals1[CGN::UNASS].end() )
    return Conn::CAUSAAL;

  return Conn::NOCONN;
}

Situation::Type wordStats::checkSituation() const {
  if ( settings.time_sits[tag].find( lemma )
       != settings.time_sits[tag].end() ){
    return Situation::TIME_SIT;
  }
  else if ( settings.time_sits[CGN::UNASS].find( lemma )
	    != settings.time_sits[CGN::UNASS].end() ){
    return Situation::TIME_SIT;
  }
  else if ( settings.causal_sits[tag].find( lemma )
	    != settings.causal_sits[tag].end() ){
    return Situation::CAUSAL_SIT;
  }
  else if ( settings.causal_sits[CGN::UNASS].find( lemma )
	    != settings.causal_sits[CGN::UNASS].end() ){
    return Situation::CAUSAL_SIT;
  }
  else if ( settings.space_sits[tag].find( lemma )
	    != settings.space_sits[tag].end() ){
    return Situation::SPACE_SIT;
  }
  else if ( settings.space_sits[CGN::UNASS].find( lemma )
	    != settings.space_sits[CGN::UNASS].end() ){
    return Situation::SPACE_SIT;
  }
  else if ( settings.emotion_sits[tag].find( lemma )
	    != settings.emotion_sits[tag].end() ){
    return Situation::EMO_SIT;
  }
  else if ( settings.emotion_sits[CGN::UNASS].find( lemma )
	    != settings.emotion_sits[CGN::UNASS].end() ){
    return Situation::EMO_SIT;
  }
  return Situation::NO_SIT;
}

void wordStats::checkNoun() {
  if ( tag == CGN::N ){
    //    cerr << "lookup " << lemma << endl;
    map<string,noun>::const_iterator sit = settings.noun_sem.find( lemma );
    if ( sit != settings.noun_sem.end() ){
      noun n = sit->second;
      sem_type = n.type;
      if (n.is_compound) {
        is_compound = n.is_compound;
        compound_parts = n.compound_parts;
        compound_head = n.head;
        compound_sat = n.satellite_clean;
      }
    }
    else {
      sem_type = SEM::UNFOUND_NOUN;
      if ( settings.showProblems ){
        problemFile << "N," << word << ", " << lemma << endl;
      }
    }
  }
}

SEM::Type wordStats::checkSemProps( ) const {
  if ( prop == CGN::ISNAME ){
    // Names are te be looked up in the Noun list too
    SEM::Type sem = SEM::UNFOUND_NOUN;
    map<string,noun>::const_iterator sit = settings.noun_sem.find( lemma );
    if ( sit != settings.noun_sem.end() ){
      sem = sit->second.type;
    }
    // else if ( settings.showProblems ){
    //   problemFile << "Name, " << word << ", " << lemma << endl;
    // }
    return sem;
  }
  else if ( tag == CGN::ADJ ) {
    //    cerr << "ADJ check semtype " << l_lemma << endl;
    SEM::Type sem = SEM::UNFOUND_ADJ;
    map<string,SEM::Type>::const_iterator sit = settings.adj_sem.find( l_lemma );
    if ( sit == settings.adj_sem.end() ){
      // lemma not found. maybe the whole word?
      //      cerr << "ADJ check semtype " << word << endl;
      sit = settings.adj_sem.find( l_word );
    }
    if ( sit != settings.adj_sem.end() ){
      sem = sit->second;
    }
    else if ( settings.showProblems ){
      problemFile << "ADJ," << l_word << "," << l_lemma << endl;
    }
    //    cerr << "found semtype " << sem << endl;
    return sem;
  }
  else if ( tag == CGN::WW ) {
    //    cerr << "check semtype " << lemma << endl;
    SEM::Type sem = SEM::UNFOUND_VERB;
    map<string,SEM::Type>::const_iterator sit = settings.verb_sem.end();
    if ( !full_lemma.empty() ) {
      sit = settings.verb_sem.find( full_lemma );
    }
    if ( sit == settings.verb_sem.end() ){
      if ( position == CGN::PRENOM
	   && ( prop == CGN::ISVD || prop == CGN::ISOD ) ){
	// might be a 'hidden' adj!
	//	cerr << "lookup a probable ADJ " << prop << " (" << word << ") " << endl;
	sit = settings.adj_sem.find( l_word );
	if ( sit == settings.adj_sem.end() )
	  sit = settings.verb_sem.end();
      }
    }
    if ( sit == settings.verb_sem.end() ){
      //      cerr << "lookup lemma as verb (" << lemma << ") " << endl;
      sit = settings.verb_sem.find( l_lemma );
    }
    if ( sit != settings.verb_sem.end() ){
      sem = sit->second;
    }
    else if ( settings.showProblems ){
      problemFile << "WW," << l_word << "," << l_lemma;
      if ( !full_lemma.empty() )
	problemFile << "," << full_lemma << endl;
      else
	problemFile << endl;
    }
    //    cerr << "found semtype " << sem << endl;
    return sem;
  }
  return SEM::NO_SEMTYPE;
}

Intensify::Type wordStats::checkIntensify(const xmlNode *alpWord) const {
  map<string,Intensify::Type>::const_iterator sit = settings.intensify.find(lemma);
  Intensify::Type res = Intensify::NO_INTENSIFY;
  if (sit != settings.intensify.end()) {
    res = sit->second;
    if (res == Intensify::BVBW)
    {
      // cerr << lemma << " => " << tag << endl;
      if (!checkModifier(alpWord)) res = Intensify::NO_INTENSIFY;
    }
  }
  return res;
}

General::Type wordStats::checkGeneralNoun() const {
  if (tag == CGN::N) {
    map<string,General::Type>::const_iterator sit = settings.general_nouns.find(lemma);
    if (sit != settings.general_nouns.end()) {
      return sit->second;
    }
  }
  return General::NO_GENERAL;
}

General::Type wordStats::checkGeneralVerb() const {
  if (tag == CGN::WW) {
    map<string,General::Type>::const_iterator sit = settings.general_verbs.find(lemma);
    if (sit != settings.general_verbs.end()) {
      return sit->second;
    }
  }
  return General::NO_GENERAL;
}

Adverb::Type checkAdverbType(string word, CGN::Type tag) {
  if (tag == CGN::BW) {
    map<string,Adverb::Type>::const_iterator sit = settings.adverbs.find(word);
    if (sit != settings.adverbs.end()) {
      return sit->second;
    }
  }
  return Adverb::NO_ADVERB;
}

Afk::Type wordStats::checkAfk() const {
  if ( tag == CGN::N || tag == CGN::SPEC) {
    map<string,Afk::Type>::const_iterator sit = settings.afkos.find( word );
    if ( sit != settings.afkos.end() ){
      return sit->second;
    }
  }
  return Afk::NO_A;
}

// Returns the position of a word in the top-20000 lexicon
top_val wordStats::topFreqLookup(const string& w) const {
  map<string,top_val>::const_iterator it = settings.top_freq_lex.find( w );
  top_val result = notFound;
  if ( it != settings.top_freq_lex.end() ){
    result = it->second;
  }
  return result;
}

// Returns the frequency of a word in the word lexicon
int wordStats::wordFreqLookup(const string& w) const {
  int result = 0;
  map<string,cf_data>::const_iterator it = settings.word_freq_lex.find( w );
  if ( it != settings.word_freq_lex.end() ){
    result = it->second.count;
  }
  return result;
}

// Returns the log of the frequency per billion words (with Laplace transformation)
// See http://crr.ugent.be/papers/van_Heuven_et_al_SUBTLEX-UK.pdf
double freqLog(const long int& freq, const long int& total) {
  return log10(((freq + 1) / double(total)) * 1e9);
}

// Find the frequencies of words and lemmata
void wordStats::freqLookup(){
  word_freq = wordFreqLookup(l_word);
  word_freq_log = freqLog(word_freq, settings.word_total);

  map<string,cf_data>::const_iterator it = settings.lemma_freq_lex.end();
  if ( !full_lemma.empty() ){
    // scheidbaar ww
    it = settings.lemma_freq_lex.find( full_lemma );
  }
  if ( it == settings.lemma_freq_lex.end() ){
    it = settings.lemma_freq_lex.find( l_lemma );
  }
  if ( it != settings.lemma_freq_lex.end() ){
    lemma_freq = it->second.count;
    lemma_freq_log = freqLog(lemma_freq, settings.lemma_total);
  }
  else {
    lemma_freq = 0;
    lemma_freq_log = freqLog(lemma_freq, settings.lemma_total);
  }
}

void wordStats::staphFreqLookup(){
  map<string,cf_data>::const_iterator it = settings.staph_word_freq_lex.find( l_word );
  if ( it != settings.staph_word_freq_lex.end() ){
    double freq = it->second.freq;
    if ( freq <= 50 )
      f50 = true;
    if ( freq <= 65 )
      f65 = true;
    if ( freq <= 77 )
      f77 = true;
    if ( freq <= 80 )
      f80 = true;
  }
}

void argument_overlap( const string w_or_l,
		       const vector<string>& buffer,
		       int& arg_overlap_cnt ){
  // calculate the overlap of the Word or Lemma with the buffer
  if ( buffer.empty() )
    return;
  // cerr << "test overlap, lemma/word= " << w_or_l << endl;
  // cerr << "buffer=" << buffer << endl;
  static string vnw_1sA[] = {"ik", "mij", "me", "mijn" };
  static set<string> vnw_1s = set<string>( vnw_1sA,
					   vnw_1sA + sizeof(vnw_1sA)/sizeof(string) );
  static string vnw_2sA[] = {"jij", "je", "jou", "jouw" };
  static set<string> vnw_2s = set<string>( vnw_2sA,
					   vnw_2sA + sizeof(vnw_2sA)/sizeof(string) );
  static string vnw_3smA[] = {"hij", "hem", "zijn" };
  static set<string> vnw_3sm = set<string>( vnw_3smA,
					    vnw_3smA + sizeof(vnw_3smA)/sizeof(string) );
  static string vnw_3sfA[] = {"zij", "ze", "haar"};
  static set<string> vnw_3sf = set<string>( vnw_3sfA,
					    vnw_3sfA + sizeof(vnw_3sfA)/sizeof(string) );
  static string vnw_1pA[] = {"wij", "we", "ons", "onze"};
  static set<string> vnw_1p = set<string>( vnw_1pA,
					   vnw_1pA + sizeof(vnw_1pA)/sizeof(string) );
  static string vnw_2pA[] = {"jullie"};
  static set<string> vnw_2p = set<string>( vnw_2pA,
					   vnw_2pA + sizeof(vnw_2pA)/sizeof(string) );
  static string vnw_3pA[] = {"zij", "ze", "hen", "hun"};
  static set<string> vnw_3p = set<string>( vnw_3pA,
					   vnw_3pA + sizeof(vnw_3pA)/sizeof(string) );

  for( size_t i=0; i < buffer.size(); ++i ){
    if ( w_or_l == buffer[i] ){
      ++arg_overlap_cnt;
      break;
    }
    else if ( vnw_1s.find( w_or_l ) != vnw_1s.end() &&
	      vnw_1s.find( buffer[i] ) != vnw_1s.end() ){
      ++arg_overlap_cnt;
      break;
    }
    else if ( vnw_2s.find( w_or_l ) != vnw_2s.end() &&
	      vnw_2s.find( buffer[i] ) != vnw_2s.end() ){
      ++arg_overlap_cnt;
      break;
    }
    else if ( vnw_3sm.find( w_or_l ) != vnw_3sm.end() &&
	      vnw_3sm.find( buffer[i] ) != vnw_3sm.end() ){
      ++arg_overlap_cnt;
      break;
    }
    else if ( vnw_3sf.find( w_or_l ) != vnw_3sf.end() &&
	      vnw_3sf.find( buffer[i] ) != vnw_3sf.end() ){
      ++arg_overlap_cnt;
      break;
    }
    else if ( vnw_1p.find( w_or_l ) != vnw_1p.end() &&
	      vnw_1p.find( buffer[i] ) != vnw_1p.end() ){
      ++arg_overlap_cnt;
       break;
   }
    else if ( vnw_2p.find( w_or_l ) != vnw_2p.end() &&
	      vnw_2p.find( buffer[i] ) != vnw_2p.end() ){
      ++arg_overlap_cnt;
      break;
    }
    else if ( vnw_3p.find( w_or_l ) != vnw_3p.end() &&
	      vnw_3p.find( buffer[i] ) != vnw_3p.end() ){
      ++arg_overlap_cnt;
      break;
    }
  }
}

wordStats::wordStats( int index,
		      folia::Word *w,
		      const xmlNode *alpWord,
		      const set<size_t>& puncts,
		      bool fail ):
  basicStats( index, w, "word" ), parseFail(fail), wwform(::NO_VERB),
  isPersRef(false), isPronRef(false),
  archaic(false), isContent(false), isNominal(false),isOnder(false), isImperative(false),
  isBetr(false), isPropNeg(false), isMorphNeg(false),
  nerProp(NER::NONER), connType(Conn::NOCONN), isMultiConn(false), sitType(Situation::NO_SIT),
  f50(false), f65(false), f77(false), f80(false),
  top_freq(notFound), word_freq(0), lemma_freq(0),
  wordOverlapCnt(0), lemmaOverlapCnt(0),
  word_freq_log(NAN), lemma_freq_log(NAN),
  logprob10(NAN), prop(CGN::JUSTAWORD), position(CGN::NOPOS),
  sem_type(SEM::NO_SEMTYPE), intensify_type(Intensify::NO_INTENSIFY),
  general_noun_type(General::NO_GENERAL), general_verb_type(General::NO_GENERAL),
  adverb_type(Adverb::NO_ADVERB), afkType(Afk::NO_A), is_compound(false), compound_parts(0),
  word_freq_log_head(NAN), word_freq_log_sat(NAN), word_freq_log_head_sat(NAN)
{
  UnicodeString us = w->text();
  charCnt = us.length();
  word = folia::UnicodeToUTF8( us );
  l_word = folia::UnicodeToUTF8( us.toLower() );
  if ( fail )
    return;
  vector<folia::PosAnnotation*> posV = w->select<folia::PosAnnotation>(frog_pos_set);
  if ( posV.size() != 1 )
    throw folia::ValueError( "word doesn't have Frog POS tag info" );
  folia::PosAnnotation *pa = posV[0];
  pos = pa->cls();
  tag = CGN::toCGN( pa->feat("head") );
  lemma = w->lemma( frog_lemma_set );
  us = folia::UTF8ToUnicode( lemma );
  l_lemma = folia::UnicodeToUTF8( us.toLower() );

  setCGNProps( pa );
  if ( alpWord ){
    distances = getDependencyDist( alpWord, puncts);
    if ( tag == CGN::WW ){
      string full;
      wwform = classifyVerb( alpWord, lemma, full );
      if ( !full.empty() ){
	to_lower( full );
	//	cerr << "scheidbaar WW: " << full << endl;
	full_lemma = full;
      }
      if ( (prop == CGN::ISPVTGW || prop == CGN::ISPVVERL) &&
	   wwform != PASSIVE_VERB ){
	isImperative = checkImp( alpWord );
      }
    }
  }
  isContent = checkContent();
  if ( prop != CGN::ISLET ){
    vector<string> mv = get_full_morph_analysis( w, true );
    // get_full_morph_amalysis returns 1 or more morpheme sequences
    // like [appel][taart] of [veilig][heid]
    // there may be more readings/morpheme lists:
    // [ge][naken][t] versus [genaak][t]
    size_t max = 0;
    size_t pos = 0;
    size_t match_pos = 0;
    for ( auto const s : mv ){
      vector<string> parts;
      TiCC::split_at_first_of( s, parts, "[]" );
      if ( parts.size() > max ){
	// a hack: we assume the longest morpheme list to
	// be the best choice.
	morphemes = parts;
	max = parts.size();
	match_pos = pos;
      }
      ++pos;
    }
    if ( morphemes.size() == 0 ){
      cerr << "unable to retrieve morphemes from folia." << endl;
    }
    //    cerr << "Morphemes " << word << "= " << morphemes << endl;
    vector<string> cmps = get_compound_analysis(w);
    //    cerr << "Comps " << word << "= " << cmps << endl;
    if ( cmps.size() > match_pos ) {
      // this might not be the case e.g. when frog isn't started
      // with the --deep-morph option!
      compstr = cmps[match_pos];
    }
    isPropNeg = checkPropNeg();
    isMorphNeg = checkMorphNeg();
    connType = checkConnective();
    sitType = checkSituation();
    morphCnt = morphemes.size();
    if ( prop != CGN::ISNAME ){
      charCntExNames = charCnt;
      morphCntExNames = morphCnt;
    }
    sem_type = checkSemProps();
    checkNoun();
    intensify_type = checkIntensify(alpWord);
    general_noun_type = checkGeneralNoun();
    general_verb_type = checkGeneralVerb();
    adverb_type = checkAdverbType(word, tag);
    afkType = checkAfk();
    if ( alpWord )
      isNominal = checkNominal( alpWord );
    top_freq = topFreqLookup(l_word);
    staphFreqLookup();
    if ( isContent ){
      freqLookup();
    }
    if ( is_compound ) {
      charCntHead = compound_head.length();
      charCntSat = compound_sat.length();
      word_freq_log_head = freqLog(wordFreqLookup(compound_head), settings.word_total);
      word_freq_log_sat = freqLog(wordFreqLookup(compound_sat), settings.word_total);
      word_freq_log_head_sat = (word_freq_log_head + word_freq_log_sat) / double(2);
      top_freq_head = topFreqLookup(compound_head);
      top_freq_sat = topFreqLookup(compound_sat);
    }
  }
}

void addOneMetric( folia::Document *doc, folia::FoliaElement *parent,
		   const string& cls, const string& val ){
  folia::Metric *m = new folia::Metric( folia::getArgs( "class='" + cls + "', value='" + val + "'" ),
			    doc );
  parent->append( m );
}

void fill_word_lemma_buffers( const sentStats*,
			      vector<string>&, vector<string>& );


//#define DEBUG_OL

bool wordStats::isOverlapCandidate() const {
  if ( ( tag == CGN::VNW && prop != CGN::ISAANW ) ||
       ( tag == CGN::N ) ||
       ( prop == CGN::ISNAME ) ||
       ( tag == CGN::WW && wwform == HEAD_VERB ) ){
    return true;
  }
  else {
#ifdef DEBUG_OL
    if ( tag == CGN::WW ){
      cerr << "WW overlapcandidate REJECTED " << toString(wwform) << " " << word << endl;
    }
    else if ( tag == CGN::VNW ){
      cerr << "VNW overlapcandidate REJECTED " << toString(prop) << " " << word << endl;
    }
#endif
    return false;
  }
}
//#undef DEBUG_OL

void wordStats::getSentenceOverlap( const vector<string>& wordbuffer,
				    const vector<string>& lemmabuffer ){
  if ( isOverlapCandidate() ){
    // get the words and lemmas' of the previous sentence
#ifdef DEBUG_OL
    cerr << "call word sentenceOverlap, word = " << l_word;
    int tmp2 = wordOverlapCnt;
#endif
    argument_overlap( l_word, wordbuffer, wordOverlapCnt );
#ifdef DEBUG_OL
    if ( tmp2 != wordOverlapCnt ){
      cerr << " OVERLAPPED " << endl;
    }
    else
      cerr << endl;
    cerr << "call lemma sentenceOverlap, lemma= " << l_lemma;
    tmp2 = lemmaOverlapCnt;
#endif
    argument_overlap( l_lemma, lemmabuffer, lemmaOverlapCnt );
#ifdef DEBUG_OL
    if ( tmp2 != lemmaOverlapCnt ){
      cerr << " OVERLAPPED " << endl;
    }
    else
      cerr << endl;
#endif
  }
}

void wordStats::addMetrics( ) const {
  folia::FoliaElement *el = folia_node;
  folia::Document *doc = el->doc();
  if ( wwform != ::NO_VERB ){
    folia::KWargs args;
    args["set"] = "tscan-set";
    args["class"] = "wwform(" + toString(wwform) + ")";
    el->addPosAnnotation( args );
  }
  if ( !full_lemma.empty() ){
    addOneMetric( doc, el, "full-lemma", full_lemma );
  }
  if ( isPersRef )
    addOneMetric( doc, el, "pers_ref", "true" );
  if ( isPronRef )
    addOneMetric( doc, el, "pron_ref", "true" );
  if ( archaic )
    addOneMetric( doc, el, "archaic", "true" );
  if ( isContent )
    addOneMetric( doc, el, "content_word", "true" );
  if ( isNominal )
    addOneMetric( doc, el, "nominalization", "true" );
  if ( isOnder )
    addOneMetric( doc, el, "subordinate", "true" );
  if ( isImperative )
    addOneMetric( doc, el, "imperative", "true" );
  if ( isBetr )
    addOneMetric( doc, el, "betrekkelijk", "true" );
  if ( isPropNeg )
    addOneMetric( doc, el, "proper_negative", "true" );
  if ( isMorphNeg )
    addOneMetric( doc, el, "morph_negative", "true" );
  if ( connType != Conn::NOCONN )
    addOneMetric( doc, el, "connective", toString(connType) );
  if ( sitType != Situation::NO_SIT )
    addOneMetric( doc, el, "situation", toString(sitType) );
  if ( isMultiConn )
    addOneMetric( doc, el, "multi_connective", "true" );
  if ( lsa_opv )
    addOneMetric( doc, el, "lsa_word_suc", toString(lsa_opv) );
  if ( lsa_ctx )
    addOneMetric( doc, el, "lsa_word_ctx", toString(lsa_ctx) );
  if ( f50 )
    addOneMetric( doc, el, "f50", "true" );
  if ( f65 )
    addOneMetric( doc, el, "f65", "true" );
  if ( f77 )
    addOneMetric( doc, el, "f77", "true" );
  if ( f80 )
    addOneMetric( doc, el, "f80", "true" );
  if ( top_freq == top1000 )
    addOneMetric( doc, el, "top1000", "true" );
  else if ( top_freq == top2000 )
    addOneMetric( doc, el, "top2000", "true" );
  else if ( top_freq == top3000 )
    addOneMetric( doc, el, "top3000", "true" );
  else if ( top_freq == top5000 )
    addOneMetric( doc, el, "top5000", "true" );
  else if ( top_freq == top10000 )
    addOneMetric( doc, el, "top10000", "true" );
  else if ( top_freq == top20000 )
    addOneMetric( doc, el, "top20000", "true" );
  addOneMetric( doc, el, "word_freq", toString(word_freq) );
  if ( !std::isnan(word_freq_log) )
    addOneMetric( doc, el, "log_word_freq", toString(word_freq_log) );
  addOneMetric( doc, el, "lemma_freq", toString(lemma_freq) );
  if ( !std::isnan(lemma_freq_log) )
    addOneMetric( doc, el, "log_lemma_freq", toString(lemma_freq_log) );
  addOneMetric( doc, el,
		"word_overlap_count", toString( wordOverlapCnt ) );
  addOneMetric( doc, el,
		"lemma_overlap_count", toString( lemmaOverlapCnt ) );
  if ( !std::isnan(logprob10)  )
    addOneMetric( doc, el, "lprob10", toString(logprob10) );
  if ( prop != CGN::JUSTAWORD )
    addOneMetric( doc, el, "property", toString(prop) );
  if ( sem_type != SEM::NO_SEMTYPE )
    addOneMetric( doc, el, "semtype", toString(sem_type) );
  if ( intensify_type != Intensify::NO_INTENSIFY )
    addOneMetric( doc, el, "intensifytype", Intensify::toString(intensify_type) );
  if ( general_noun_type != General::NO_GENERAL )
    addOneMetric( doc, el, "generalnountype", General::toString(general_noun_type) );
  if ( general_verb_type != General::NO_GENERAL )
    addOneMetric( doc, el, "generalverbtype", General::toString(general_verb_type) );
  if ( adverb_type != Adverb::NO_ADVERB )
    addOneMetric( doc, el, "adverbtype", Adverb::toString(adverb_type) );
  if ( afkType != Afk::NO_A )
    addOneMetric( doc, el, "afktype", toString(afkType) );
}

void structStats::merge( structStats *ss ){
  if ( ss->parseFailCnt == -1 ) // not parsed
    parseFailCnt = -1;
  else
    parseFailCnt += ss->parseFailCnt;
  wordCnt += ss->wordCnt;
  if ( ss->wordCnt != 0 ) // don't count sentences without words
    sentCnt += ss->sentCnt;
  charCnt += ss->charCnt;
  charCntExNames += ss->charCntExNames;
  morphCnt += ss->morphCnt;
  morphCntExNames += ss->morphCntExNames;
  nameCnt += ss->nameCnt;
  infBvCnt += ss->infBvCnt;
  infNwCnt += ss->infNwCnt;
  infVrijCnt += ss->infVrijCnt;
  vdBvCnt += ss->vdBvCnt;
  vdNwCnt += ss->vdNwCnt;
  vdVrijCnt += ss->vdVrijCnt;
  odBvCnt += ss->odBvCnt;
  odNwCnt += ss->odNwCnt;
  odVrijCnt += ss->odVrijCnt;
  passiveCnt += ss->passiveCnt;
  modalCnt += ss->modalCnt;
  timeVCnt += ss->timeVCnt;
  koppelCnt += ss->koppelCnt;
  archaicsCnt += ss->archaicsCnt;
  contentCnt += ss->contentCnt;
  nominalCnt += ss->nominalCnt;
  adjCnt += ss->adjCnt;
  vgCnt += ss->vgCnt;
  vnwCnt += ss->vnwCnt;
  lidCnt += ss->lidCnt;
  vzCnt += ss->vzCnt;
  bwCnt += ss->bwCnt;
  twCnt += ss->twCnt;
  nounCnt += ss->nounCnt;
  verbCnt += ss->verbCnt;
  tswCnt += ss->tswCnt;
  specCnt += ss->specCnt;
  letCnt += ss->letCnt;
  betrCnt += ss->betrCnt;
  bijwCnt += ss->bijwCnt;
  complCnt += ss->complCnt;
  mvFinInbedCnt += ss->mvFinInbedCnt;
  infinComplCnt += ss->infinComplCnt;
  mvInbedCnt += ss->mvInbedCnt;
  losBetrCnt += ss->losBetrCnt;
  losBijwCnt += ss->losBijwCnt;
  tempConnCnt += ss->tempConnCnt;
  opsomWgConnCnt += ss->opsomWgConnCnt;
  opsomZinConnCnt += ss->opsomZinConnCnt;
  contrastConnCnt += ss->contrastConnCnt;
  compConnCnt += ss->compConnCnt;
  causeConnCnt += ss->causeConnCnt;
  timeSitCnt += ss->timeSitCnt;
  spaceSitCnt += ss->spaceSitCnt;
  causeSitCnt += ss->causeSitCnt;
  emoSitCnt += ss->emoSitCnt;
  prepExprCnt += ss->prepExprCnt;
  propNegCnt += ss->propNegCnt;
  morphNegCnt += ss->morphNegCnt;
  multiNegCnt += ss->multiNegCnt;
  wordOverlapCnt += ss->wordOverlapCnt;
  lemmaOverlapCnt += ss->lemmaOverlapCnt;
  f50Cnt += ss->f50Cnt;
  f65Cnt += ss->f65Cnt;
  f77Cnt += ss->f77Cnt;
  f80Cnt += ss->f80Cnt;
  top1000Cnt += ss->top1000Cnt;
  top2000Cnt += ss->top2000Cnt;
  top3000Cnt += ss->top3000Cnt;
  top5000Cnt += ss->top5000Cnt;
  top10000Cnt += ss->top10000Cnt;
  top20000Cnt += ss->top20000Cnt;
  top1000ContentCnt += ss->top1000ContentCnt;
  top2000ContentCnt += ss->top2000ContentCnt;
  top3000ContentCnt += ss->top3000ContentCnt;
  top5000ContentCnt += ss->top5000ContentCnt;
  top10000ContentCnt += ss->top10000ContentCnt;
  top20000ContentCnt += ss->top20000ContentCnt;
  word_freq += ss->word_freq;
  word_freq_n += ss->word_freq_n;
  lemma_freq += ss->lemma_freq;
  lemma_freq_n += ss->lemma_freq_n;
  if ( !std::isnan(ss->avg_prob10) ){
    if ( std::isnan(avg_prob10) )
      avg_prob10 = ss->avg_prob10;
    else
      avg_prob10 += ss->avg_prob10;
  }
  if ( !std::isnan(ss->entropy) ){
    if ( std::isnan(entropy) )
      entropy = ss->entropy;
    else
      entropy += ss->entropy;
  }
  if ( !std::isnan(ss->perplexity) ){
    if ( std::isnan(perplexity) )
      perplexity = ss->perplexity;
    else
      perplexity += ss->perplexity;
  }

  intensCnt += ss->intensCnt;
  intensBvnwCnt += ss->intensBvnwCnt;
  intensBvbwCnt += ss->intensBvbwCnt;
  intensBwCnt += ss->intensBwCnt;
  intensCombiCnt += ss->intensCombiCnt;
  intensNwCnt += ss->intensNwCnt;
  intensTussCnt += ss->intensTussCnt;
  intensWwCnt += ss->intensWwCnt;
  generalNounCnt += ss->generalNounCnt;
  generalNounSepCnt += ss->generalNounSepCnt;
  generalNounRelCnt += ss->generalNounRelCnt;
  generalNounActCnt += ss->generalNounActCnt;
  generalNounKnowCnt += ss->generalNounKnowCnt;
  generalNounDiscCnt += ss->generalNounDiscCnt;
  generalNounDeveCnt += ss->generalNounDeveCnt;
  generalVerbCnt += ss->generalVerbCnt;
  generalVerbSepCnt += ss->generalVerbSepCnt;
  generalVerbRelCnt += ss->generalVerbRelCnt;
  generalVerbActCnt += ss->generalVerbActCnt;
  generalVerbKnowCnt += ss->generalVerbKnowCnt;
  generalVerbDiscCnt += ss->generalVerbDiscCnt;
  generalVerbDeveCnt += ss->generalVerbDeveCnt;
  generalAdverbCnt += ss->generalAdverbCnt;
  specificAdverbCnt += ss->specificAdverbCnt;
  smainCnt += ss->smainCnt;
  ssubCnt += ss->ssubCnt;
  sv1Cnt += ss->sv1Cnt;
  clauseCnt += ss->clauseCnt;
  correctedClauseCnt += ss->correctedClauseCnt;
  smainCnjCnt += ss->smainCnjCnt;
  ssubCnjCnt += ss->ssubCnjCnt;
  sv1CnjCnt += ss->sv1CnjCnt;
  presentCnt += ss->presentCnt;
  pastCnt += ss->pastCnt;
  subjonctCnt += ss->subjonctCnt;
  pron1Cnt += ss->pron1Cnt;
  pron2Cnt += ss->pron2Cnt;
  pron3Cnt += ss->pron3Cnt;
  persRefCnt += ss->persRefCnt;
  pronRefCnt += ss->pronRefCnt;
  strictNounCnt += ss->strictNounCnt;
  broadNounCnt += ss->broadNounCnt;
  strictAdjCnt += ss->strictAdjCnt;
  broadAdjCnt += ss->broadAdjCnt;
  subjectiveAdjCnt += ss->subjectiveAdjCnt;
  abstractWwCnt += ss->abstractWwCnt;
  concreteWwCnt += ss->concreteWwCnt;
  undefinedWwCnt += ss->undefinedWwCnt;
  undefinedATPCnt += ss->undefinedATPCnt;
  stateCnt += ss->stateCnt;
  actionCnt += ss->actionCnt;
  processCnt += ss->processCnt;
  humanAdjCnt += ss->humanAdjCnt;
  emoAdjCnt += ss->emoAdjCnt;
  nonhumanAdjCnt += ss->nonhumanAdjCnt;
  shapeAdjCnt += ss->shapeAdjCnt;
  colorAdjCnt += ss->colorAdjCnt;
  matterAdjCnt += ss->matterAdjCnt;
  soundAdjCnt += ss->soundAdjCnt;
  nonhumanOtherAdjCnt += ss->nonhumanOtherAdjCnt;
  techAdjCnt += ss->techAdjCnt;
  timeAdjCnt += ss->timeAdjCnt;
  placeAdjCnt += ss->placeAdjCnt;
  specPosAdjCnt += ss->specPosAdjCnt;
  specNegAdjCnt += ss->specNegAdjCnt;
  posAdjCnt += ss->posAdjCnt;
  negAdjCnt += ss->negAdjCnt;
  evaluativeAdjCnt += ss->evaluativeAdjCnt;
  epiPosAdjCnt += ss->epiPosAdjCnt;
  epiNegAdjCnt += ss->epiNegAdjCnt;
  abstractAdjCnt += ss->abstractAdjCnt;
  undefinedNounCnt += ss->undefinedNounCnt;
  uncoveredNounCnt += ss->uncoveredNounCnt;
  undefinedAdjCnt += ss->undefinedAdjCnt;
  uncoveredAdjCnt += ss->uncoveredAdjCnt;
  uncoveredVerbCnt += ss->uncoveredVerbCnt;
  humanCnt += ss->humanCnt;
  nonHumanCnt += ss->nonHumanCnt;
  artefactCnt += ss->artefactCnt;
  concrotherCnt += ss->concrotherCnt;
  substanceConcCnt += ss->substanceConcCnt;
  foodcareCnt += ss->foodcareCnt;
  timeCnt += ss->timeCnt;
  placeCnt += ss->placeCnt;
  measureCnt += ss->measureCnt;
  dynamicConcCnt += ss->dynamicConcCnt;
  substanceAbstrCnt += ss->substanceAbstrCnt;
  dynamicAbstrCnt += ss->dynamicAbstrCnt;
  nonDynamicCnt += ss->nonDynamicCnt;
  institutCnt += ss->institutCnt;
  npCnt += ss->npCnt;
  indefNpCnt += ss->indefNpCnt;
  npSize += ss->npSize;
  vcModCnt += ss->vcModCnt;
  vcModSingleCnt += ss->vcModSingleCnt;
  adjNpModCnt += ss->adjNpModCnt;
  npModCnt += ss->npModCnt;
  if ( ss->dLevel >= 0 ){
    if ( dLevel < 0 )
      dLevel = ss->dLevel;
    else
      dLevel += ss->dLevel;
  }
  dLevel_gt4 += ss->dLevel_gt4;
  impCnt += ss->impCnt;
  questCnt += ss->questCnt;
  nerCnt += ss->nerCnt;
  compoundCnt += ss->compoundCnt;
  compound3Cnt += ss->compound3Cnt;
  charCntNoun += ss->charCntNoun;
  charCntNonComp += ss->charCntNonComp;
  charCntComp += ss->charCntComp;
  charCntHead += ss->charCntHead;
  charCntSat += ss->charCntSat;
  charCntNounCorr += ss->charCntNounCorr;
  charCntCorr += ss->charCntCorr;
  word_freq_log_noun += ss->word_freq_log_noun;
  word_freq_log_non_comp += ss->word_freq_log_non_comp;
  word_freq_log_comp += ss->word_freq_log_comp;
  word_freq_log_head += ss->word_freq_log_head;
  word_freq_log_sat += ss->word_freq_log_sat;
  word_freq_log_head_sat += ss->word_freq_log_head_sat;
  word_freq_log_noun_corr += ss->word_freq_log_noun_corr;
  word_freq_log_corr += ss->word_freq_log_corr;
  top1000CntNoun += ss->top1000CntNoun;
  top1000CntNonComp += ss->top1000CntNonComp;
  top1000CntComp += ss->top1000CntComp;
  top1000CntHead += ss->top1000CntHead;
  top1000CntSat += ss->top1000CntSat;
  top1000CntNounCorr += ss->top1000CntNounCorr;
  top1000CntCorr += ss->top1000CntCorr;
  top5000CntNoun += ss->top5000CntNoun;
  top5000CntNonComp += ss->top5000CntNonComp;
  top5000CntComp += ss->top5000CntComp;
  top5000CntHead += ss->top5000CntHead;
  top5000CntSat += ss->top5000CntSat;
  top5000CntNounCorr += ss->top5000CntNounCorr;
  top5000CntCorr += ss->top5000CntCorr;
  top20000CntNoun += ss->top20000CntNoun;
  top20000CntNonComp += ss->top20000CntNonComp;
  top20000CntComp += ss->top20000CntComp;
  top20000CntHead += ss->top20000CntHead;
  top20000CntSat += ss->top20000CntSat;
  top20000CntNounCorr += ss->top20000CntNounCorr;
  top20000CntCorr += ss->top20000CntCorr;
  sv.push_back( ss );
  aggregate( heads, ss->heads );
  aggregate( unique_names, ss->unique_names );
  aggregate( unique_contents, ss->unique_contents );
  aggregate( unique_words, ss->unique_words );
  aggregate( unique_lemmas, ss->unique_lemmas );
  aggregate( unique_tijd_sits, ss->unique_tijd_sits );
  aggregate( unique_ruimte_sits, ss->unique_ruimte_sits );
  aggregate( unique_cause_sits, ss->unique_cause_sits );
  aggregate( unique_emotion_sits, ss->unique_emotion_sits );
  aggregate( unique_temp_conn, ss->unique_temp_conn );
  aggregate( unique_reeks_wg_conn, ss->unique_reeks_wg_conn );
  aggregate( unique_reeks_zin_conn, ss->unique_reeks_zin_conn );
  aggregate( unique_contr_conn, ss->unique_contr_conn );
  aggregate( unique_comp_conn, ss->unique_comp_conn );
  aggregate( unique_cause_conn, ss->unique_cause_conn );
  aggregate( ners, ss->ners );
  aggregate( afks, ss->afks );
  aggregate( distances, ss->distances );
  al_gem = getMeanAL();
  al_max = getHighestAL();
}

string MMtoString( const multimap<DD_type, int>& mm, DD_type t ){
  size_t len = mm.count(t);
  if ( len > 0 ){
    int result = 0;
    for( multimap<DD_type, int>::const_iterator pos = mm.lower_bound(t);
	 pos != mm.upper_bound(t);
	 ++pos ){
      result += pos->second;
    }
    return toString( result/double(len) );
  }
  else
    return "NA";
}

string MMtoString( const multimap<DD_type, int>& mm ){
  size_t len = mm.size();
  if ( len > 0 ){
    int result = 0;
    for( multimap<DD_type, int>::const_iterator pos = mm.begin();
	 pos != mm.end();
	 ++pos ){
      if ( !std::isnan(pos->second) )
	result += pos->second;
    }
    cerr << "MM to string " << result << "/" << len << endl;
    return toString( result/double(len) );
  }
  else
    return "NA";
}

template <class T>
int at( const map<T,int>& m, const T key ){
  typename map<T,int>::const_iterator it = m.find( key );
  if ( it != m.end() )
    return it->second;
  else
    return 0;
}

string toMString( double d ){
  if ( std::isnan(d) )
    return "NA";
  else
    return toString( d );
}

void structStats::addMetrics( ) const {
  folia::FoliaElement *el = folia_node;
  folia::Document *doc = el->doc();
  addOneMetric( doc, el, "word_count", toString(wordCnt) );
  addOneMetric( doc, el, "bv_vd_count", toString(vdBvCnt) );
  addOneMetric( doc, el, "nw_vd_count", toString(vdNwCnt) );
  addOneMetric( doc, el, "vrij_vd_count", toString(vdVrijCnt) );
  addOneMetric( doc, el, "bv_od_count", toString(odBvCnt) );
  addOneMetric( doc, el, "nw_od_count", toString(odNwCnt) );
  addOneMetric( doc, el, "vrij_od_count", toString(odVrijCnt) );
  addOneMetric( doc, el, "bv_inf_count", toString(infBvCnt) );
  addOneMetric( doc, el, "nw_inf_count", toString(infNwCnt) );
  addOneMetric( doc, el, "vrij_inf_count", toString(infVrijCnt) );
  addOneMetric( doc, el, "smain_count", toString(smainCnt) );
  addOneMetric( doc, el, "ssub_count", toString(ssubCnt) );
  addOneMetric( doc, el, "sv1_count", toString(sv1Cnt) );
  addOneMetric( doc, el, "smain_cnj_count", toString(smainCnjCnt) );
  addOneMetric( doc, el, "ssub_cnj_count", toString(ssubCnjCnt) );
  addOneMetric( doc, el, "sv1_cnj_count", toString(sv1CnjCnt) );
  addOneMetric( doc, el, "present_verb_count", toString(presentCnt) );
  addOneMetric( doc, el, "past_verb_count", toString(pastCnt) );
  addOneMetric( doc, el, "subjonct_count", toString(subjonctCnt) );
  addOneMetric( doc, el, "name_count", toString(nameCnt) );
  int val = at( ners, NER::PER_B );
  addOneMetric( doc, el, "personal_name_count", toString(val) );
  val = at( ners, NER::LOC_B );
  addOneMetric( doc, el, "location_name_count", toString(val) );
  val = at( ners, NER::ORG_B );
  addOneMetric( doc, el, "organization_name_count", toString(val) );
  val = at( ners, NER::PRO_B );
  addOneMetric( doc, el, "product_name_count", toString(val) );
  val = at( ners, NER::EVE_B );
  addOneMetric( doc, el, "event_name_count", toString(val) );
  val = at( afks, Afk::OVERHEID_A );
  if ( val > 0 ){
    addOneMetric( doc, el, "overheid_afk_count", toString(val) );
  }
  val = at( afks, Afk::JURIDISCH_A );
  if ( val > 0 ){
    addOneMetric( doc, el, "juridisch_afk_count", toString(val) );
  }
  val = at( afks, Afk::ONDERWIJS_A );
  if ( val > 0 ){
    addOneMetric( doc, el, "onderwijs_afk_count", toString(val) );
  }
  val = at( afks, Afk::MEDIA_A );
  if ( val > 0 ){
    addOneMetric( doc, el, "media_afk_count", toString(val) );
  }
  val = at( afks, Afk::GENERIEK_A );
  if ( val > 0 ){
    addOneMetric( doc, el, "generiek_afk_count", toString(val) );
  }
  val = at( afks, Afk::OVERIGE_A );
  if ( val > 0 ){
    addOneMetric( doc, el, "overige_afk_count", toString(val) );
  }
  val = at( afks, Afk::INTERNATIONAAL_A );
  if ( val > 0 ){
    addOneMetric( doc, el, "internationaal_afk_count", toString(val) );
  }
  val = at( afks, Afk::ZORG_A );
  if ( val > 0 ){
    addOneMetric( doc, el, "zorg_afk_count", toString(val) );
  }

  addOneMetric( doc, el, "pers_pron_1_count", toString(pron1Cnt) );
  addOneMetric( doc, el, "pers_pron_2_count", toString(pron2Cnt) );
  addOneMetric( doc, el, "pers_pron_3_count", toString(pron3Cnt) );
  addOneMetric( doc, el, "passive_count", toString(passiveCnt) );
  addOneMetric( doc, el, "modal_count", toString(modalCnt) );
  addOneMetric( doc, el, "time_count", toString(timeVCnt) );
  addOneMetric( doc, el, "koppel_count", toString(koppelCnt) );
  addOneMetric( doc, el, "pers_ref_count", toString(persRefCnt) );
  addOneMetric( doc, el, "pron_ref_count", toString(pronRefCnt) );
  addOneMetric( doc, el, "archaic_count", toString(archaicsCnt) );
  addOneMetric( doc, el, "content_count", toString(contentCnt) );
  addOneMetric( doc, el, "nominal_count", toString(nominalCnt) );
  addOneMetric( doc, el, "adj_count", toString(adjCnt) );
  addOneMetric( doc, el, "vg_count", toString(vgCnt) );
  addOneMetric( doc, el, "vnw_count", toString(vnwCnt) );
  addOneMetric( doc, el, "lid_count", toString(lidCnt) );
  addOneMetric( doc, el, "vz_count", toString(vzCnt) );
  addOneMetric( doc, el, "bw_count", toString(bwCnt) );
  addOneMetric( doc, el, "tw_count", toString(twCnt) );
  addOneMetric( doc, el, "noun_count", toString(nounCnt) );
  addOneMetric( doc, el, "verb_count", toString(verbCnt) );
  addOneMetric( doc, el, "tsw_count", toString(tswCnt) );
  addOneMetric( doc, el, "spec_count", toString(specCnt) );
  addOneMetric( doc, el, "let_count", toString(letCnt) );
  addOneMetric( doc, el, "rel_count", toString(betrCnt) );
  addOneMetric( doc, el, "temporal_connector_count", toString(tempConnCnt) );
  addOneMetric( doc, el, "reeks_wg_connector_count", toString(opsomWgConnCnt) );
  addOneMetric( doc, el, "reeks_zin_connector_count", toString(opsomZinConnCnt) );
  addOneMetric( doc, el, "contrast_connector_count", toString(contrastConnCnt) );
  addOneMetric( doc, el, "comparatief_connector_count", toString(compConnCnt) );
  addOneMetric( doc, el, "causaal_connector_count", toString(causeConnCnt) );
  addOneMetric( doc, el, "time_situation_count", toString(timeSitCnt) );
  addOneMetric( doc, el, "space_situation_count", toString(spaceSitCnt) );
  addOneMetric( doc, el, "cause_situation_count", toString(causeSitCnt) );
  addOneMetric( doc, el, "emotion_situation_count", toString(emoSitCnt) );
  addOneMetric( doc, el, "prop_neg_count", toString(propNegCnt) );
  addOneMetric( doc, el, "morph_neg_count", toString(morphNegCnt) );
  addOneMetric( doc, el, "multiple_neg_count", toString(multiNegCnt) );
  addOneMetric( doc, el, "voorzetsel_expression_count", toString(prepExprCnt) );
  addOneMetric( doc, el,
		"word_overlap_count", toString( wordOverlapCnt ) );
  addOneMetric( doc, el,
		"lemma_overlap_count", toString( lemmaOverlapCnt ) );
  if ( lsa_opv )
    addOneMetric( doc, el, "lsa_" + category + "_suc", toString(lsa_opv) );
  if ( lsa_ctx )
    addOneMetric( doc, el, "lsa_" + category + "_ctx", toString(lsa_ctx) );
  if ( !std::isnan(lsa_word_suc) )
    addOneMetric( doc, el, "lsa_word_suc_avg", toString(lsa_word_suc) );
  if ( !std::isnan(lsa_word_net) )
    addOneMetric( doc, el, "lsa_word_net_avg", toString(lsa_word_net) );
  if ( !std::isnan(lsa_sent_suc) )
    addOneMetric( doc, el, "lsa_sent_suc_avg", toString(lsa_sent_suc) );
  if ( !std::isnan(lsa_sent_net) )
    addOneMetric( doc, el, "lsa_sent_net_avg", toString(lsa_sent_net) );
  if ( !std::isnan(lsa_sent_ctx) )
    addOneMetric( doc, el, "lsa_sent_ctx_avg", toString(lsa_sent_ctx) );
  if ( !std::isnan(lsa_par_suc) )
    addOneMetric( doc, el, "lsa_par_suc_avg", toString(lsa_par_suc) );
  if ( !std::isnan(lsa_par_net) )
    addOneMetric( doc, el, "lsa_par_net_avg", toString(lsa_par_net) );
  if ( !std::isnan(lsa_par_ctx) )
    addOneMetric( doc, el, "lsa_par_ctx_avg", toString(lsa_par_ctx) );
  addOneMetric( doc, el, "freq50", toString(f50Cnt) );
  addOneMetric( doc, el, "freq65", toString(f65Cnt) );
  addOneMetric( doc, el, "freq77", toString(f77Cnt) );
  addOneMetric( doc, el, "freq80", toString(f80Cnt) );
  addOneMetric( doc, el, "top1000", toString(top1000Cnt) );
  addOneMetric( doc, el, "top2000", toString(top2000Cnt) );
  addOneMetric( doc, el, "top3000", toString(top3000Cnt) );
  addOneMetric( doc, el, "top5000", toString(top5000Cnt) );
  addOneMetric( doc, el, "top10000", toString(top10000Cnt) );
  addOneMetric( doc, el, "top20000", toString(top20000Cnt) );
  addOneMetric( doc, el, "top1000Content", toString(top1000ContentCnt) );
  addOneMetric( doc, el, "top2000Content", toString(top2000ContentCnt) );
  addOneMetric( doc, el, "top3000Content", toString(top3000ContentCnt) );
  addOneMetric( doc, el, "top5000Content", toString(top5000ContentCnt) );
  addOneMetric( doc, el, "top10000Content", toString(top10000ContentCnt) );
  addOneMetric( doc, el, "top20000Content", toString(top20000ContentCnt) );
  addOneMetric( doc, el, "word_freq", toString(word_freq) );
  addOneMetric( doc, el, "word_freq_no_names", toString(word_freq_n) );
  if ( !std::isnan(word_freq_log)  )
    addOneMetric( doc, el, "log_word_freq", toString(word_freq_log) );
  if ( !std::isnan(word_freq_log_n)  )
    addOneMetric( doc, el, "log_word_freq_no_names", toString(word_freq_log_n) );
  addOneMetric( doc, el, "lemma_freq", toString(lemma_freq) );
  addOneMetric( doc, el, "lemma_freq_no_names", toString(lemma_freq_n) );
  if ( !std::isnan(lemma_freq_log)  )
    addOneMetric( doc, el, "log_lemma_freq", toString(lemma_freq_log) );
  if ( !std::isnan(lemma_freq_log_n)  )
    addOneMetric( doc, el, "log_lemma_freq_no_names", toString(lemma_freq_log_n) );
  if ( !std::isnan(avg_prob10) )
    addOneMetric( doc, el, "wopr_logprob", toString(avg_prob10) );
  if ( !std::isnan(entropy) )
    addOneMetric( doc, el, "wopr_entropy", toString(entropy) );
  if ( !std::isnan(perplexity) )
    addOneMetric( doc, el, "wopr_perplexity", toString(perplexity) );

  addOneMetric( doc, el, "broad_adj", toString(broadAdjCnt) );
  addOneMetric( doc, el, "strict_adj", toString(strictAdjCnt) );
  addOneMetric( doc, el, "human_adj_count", toString(humanAdjCnt) );
  addOneMetric( doc, el, "emo_adj_count", toString(emoAdjCnt) );
  addOneMetric( doc, el, "nonhuman_adj_count", toString(nonhumanAdjCnt) );
  addOneMetric( doc, el, "shape_adj_count", toString(shapeAdjCnt) );
  addOneMetric( doc, el, "color_adj_count", toString(colorAdjCnt) );
  addOneMetric( doc, el, "matter_adj_count", toString(matterAdjCnt) );
  addOneMetric( doc, el, "sound_adj_count", toString(soundAdjCnt) );
  addOneMetric( doc, el, "other_nonhuman_adj_count", toString(nonhumanOtherAdjCnt) );
  addOneMetric( doc, el, "techn_adj_count", toString(techAdjCnt) );
  addOneMetric( doc, el, "time_adj_count", toString(timeAdjCnt) );
  addOneMetric( doc, el, "place_adj_count", toString(placeAdjCnt) );
  addOneMetric( doc, el, "pos_spec_adj_count", toString(specPosAdjCnt) );
  addOneMetric( doc, el, "neg_spec_adj_count", toString(specNegAdjCnt) );
  addOneMetric( doc, el, "pos_adj_count", toString(posAdjCnt) );
  addOneMetric( doc, el, "neg_adj_count", toString(negAdjCnt) );
  addOneMetric( doc, el, "evaluative_adj_count", toString(evaluativeAdjCnt) );
  addOneMetric( doc, el, "pos_epi_adj_count", toString(epiPosAdjCnt) );
  addOneMetric( doc, el, "neg_epi_adj_count", toString(epiNegAdjCnt) );
  addOneMetric( doc, el, "abstract_adj", toString(abstractAdjCnt) );
  addOneMetric( doc, el, "undefined_adj_count", toString(undefinedAdjCnt) );
  addOneMetric( doc, el, "covered_adj_count", toString(adjCnt-uncoveredAdjCnt) );
  addOneMetric( doc, el, "uncovered_adj_count", toString(uncoveredAdjCnt) );

  addOneMetric( doc, el, "intens_count", toString(intensCnt) );
  addOneMetric( doc, el, "intens_bvnw_count", toString(intensBvnwCnt) );
  addOneMetric( doc, el, "intens_bvbw_count", toString(intensBvbwCnt) );
  addOneMetric( doc, el, "intens_bw_count", toString(intensBwCnt) );
  addOneMetric( doc, el, "intens_combi_count", toString(intensCombiCnt) );
  addOneMetric( doc, el, "intens_nw_count", toString(intensNwCnt) );
  addOneMetric( doc, el, "intens_tuss_count", toString(intensTussCnt) );
  addOneMetric( doc, el, "intens_ww_count", toString(intensWwCnt) );

  addOneMetric( doc, el, "general_noun_count", toString(generalNounCnt) );
  addOneMetric( doc, el, "general_noun_sep_count", toString(generalNounSepCnt) );
  addOneMetric( doc, el, "general_noun_rel_count", toString(generalNounRelCnt) );
  addOneMetric( doc, el, "general_noun_act_count", toString(generalNounActCnt) );
  addOneMetric( doc, el, "general_noun_know_count", toString(generalNounKnowCnt) );
  addOneMetric( doc, el, "general_noun_disc_count", toString(generalNounDiscCnt) );
  addOneMetric( doc, el, "general_noun_deve_count", toString(generalNounDeveCnt) );

  addOneMetric( doc, el, "general_verb_count", toString(generalVerbCnt) );
  addOneMetric( doc, el, "general_verb_sep_count", toString(generalVerbSepCnt) );
  addOneMetric( doc, el, "general_verb_rel_count", toString(generalVerbRelCnt) );
  addOneMetric( doc, el, "general_verb_act_count", toString(generalVerbActCnt) );
  addOneMetric( doc, el, "general_verb_know_count", toString(generalVerbKnowCnt) );
  addOneMetric( doc, el, "general_verb_disc_count", toString(generalVerbDiscCnt) );
  addOneMetric( doc, el, "general_verb_deve_count", toString(generalVerbDeveCnt) );

  addOneMetric( doc, el, "general_adverb_count", toString(generalAdverbCnt) );
  addOneMetric( doc, el, "specific_adverb_count", toString(specificAdverbCnt) );

  addOneMetric( doc, el, "broad_noun", toString(broadNounCnt) );
  addOneMetric( doc, el, "strict_noun", toString(strictNounCnt) );
  addOneMetric( doc, el, "human_nouns_count", toString(humanCnt) );
  addOneMetric( doc, el, "nonhuman_nouns_count", toString(nonHumanCnt) );
  addOneMetric( doc, el, "artefact_nouns_count", toString(artefactCnt) );
  addOneMetric( doc, el, "concrother_nouns_count", toString(concrotherCnt) );
  addOneMetric( doc, el, "substance_conc_nouns_count", toString(substanceConcCnt) );
  addOneMetric( doc, el, "foodcare_nouns_count", toString(foodcareCnt) );
  addOneMetric( doc, el, "time_nouns_count", toString(timeCnt) );
  addOneMetric( doc, el, "place_nouns_count", toString(placeCnt) );
  addOneMetric( doc, el, "measure_nouns_count", toString(measureCnt) );
  addOneMetric( doc, el, "dynamic_conc_nouns_count", toString(dynamicConcCnt) );
  addOneMetric( doc, el, "substance_abstr_nouns_count", toString(substanceAbstrCnt) );
  addOneMetric( doc, el, "dynamic_abstr_nouns_count", toString(dynamicAbstrCnt) );
  addOneMetric( doc, el, "nondynamic_nouns_count", toString(nonDynamicCnt) );
  addOneMetric( doc, el, "institut_nouns_count", toString(institutCnt) );
  addOneMetric( doc, el, "undefined_nouns_count", toString(undefinedNounCnt) );
  addOneMetric( doc, el, "covered_nouns_count", toString(nounCnt+nameCnt-uncoveredNounCnt) );
  addOneMetric( doc, el, "uncovered_nouns_count", toString(uncoveredNounCnt) );

  addOneMetric( doc, el, "abstract_ww", toString(abstractWwCnt) );
  addOneMetric( doc, el, "concrete_ww", toString(concreteWwCnt) );
  addOneMetric( doc, el, "undefined_ww", toString(undefinedWwCnt) );
  addOneMetric( doc, el, "undefined_ATP", toString(undefinedATPCnt) );
  addOneMetric( doc, el, "state_count", toString(stateCnt) );
  addOneMetric( doc, el, "action_count", toString(actionCnt) );
  addOneMetric( doc, el, "process_count", toString(processCnt) );
  addOneMetric( doc, el, "covered_verb_count", toString(verbCnt-uncoveredVerbCnt) );
  addOneMetric( doc, el, "uncovered_verb_count", toString(uncoveredVerbCnt) );
  addOneMetric( doc, el, "indef_np_count", toString(indefNpCnt) );
  addOneMetric( doc, el, "np_count", toString(npCnt) );
  addOneMetric( doc, el, "np_size", toString(npSize) );
  addOneMetric( doc, el, "vc_modifier_count", toString(vcModCnt) );
  addOneMetric( doc, el, "vc_modifier_single_count", toString(vcModSingleCnt) );
  addOneMetric( doc, el, "adj_np_modifier_count", toString(adjNpModCnt) );
  addOneMetric( doc, el, "np_modifier_count", toString(npModCnt) );

  addOneMetric( doc, el, "character_count", toString(charCnt) );
  addOneMetric( doc, el, "character_count_min_names", toString(charCntExNames) );
  addOneMetric( doc, el, "morpheme_count", toString(morphCnt) );
  addOneMetric( doc, el, "morpheme_count_min_names", toString(morphCntExNames) );
  if ( dLevel >= 0 )
    addOneMetric( doc, el, "d_level", toString(dLevel) );
  else
    addOneMetric( doc, el, "d_level", "missing" );
  if ( dLevel_gt4 != 0 )
    addOneMetric( doc, el, "d_level_gt4", toString(dLevel_gt4) );
  if ( questCnt > 0 )
    addOneMetric( doc, el, "question_count", toString(questCnt) );
  if ( impCnt > 0 )
    addOneMetric( doc, el, "imperative_count", toString(impCnt) );
  addOneMetric( doc, el, "sub_verb_dist", MMtoString( distances, SUB_VERB ) );
  addOneMetric( doc, el, "obj_verb_dist", MMtoString( distances, OBJ1_VERB ) );
  addOneMetric( doc, el, "lijdend_verb_dist", MMtoString( distances, OBJ2_VERB ) );
  addOneMetric( doc, el, "verb_pp_dist", MMtoString( distances, VERB_PP ) );
  addOneMetric( doc, el, "noun_det_dist", MMtoString( distances, NOUN_DET ) );
  addOneMetric( doc, el, "prep_obj_dist", MMtoString( distances, PREP_OBJ1 ) );
  addOneMetric( doc, el, "verb_vc_dist", MMtoString( distances, VERB_VC ) );
  addOneMetric( doc, el, "comp_body_dist", MMtoString( distances, COMP_BODY ) );
  addOneMetric( doc, el, "crd_cnj_dist", MMtoString( distances, CRD_CNJ ) );
  addOneMetric( doc, el, "verb_comp_dist", MMtoString( distances, VERB_COMP ) );
  addOneMetric( doc, el, "noun_vc_dist", MMtoString( distances, NOUN_VC ) );
  addOneMetric( doc, el, "verb_svp_dist", MMtoString( distances, VERB_SVP ) );
  addOneMetric( doc, el, "verb_cop_dist", MMtoString( distances, VERB_PREDC_N ) );
  addOneMetric( doc, el, "verb_adj_dist", MMtoString( distances, VERB_PREDC_A ) );
  addOneMetric( doc, el, "verb_bw_mod_dist", MMtoString( distances, VERB_MOD_BW ) );
  addOneMetric( doc, el, "verb_adv_mod_dist", MMtoString( distances, VERB_MOD_A ) );
  addOneMetric( doc, el, "verb_noun_dist", MMtoString( distances, VERB_NOUN ) );

  addOneMetric( doc, el, "deplen", toMString( al_gem ) );
  addOneMetric( doc, el, "max_deplen", toMString( al_max ) );
  for ( size_t i=0; i < sv.size(); ++i ){
    sv[i]->addMetrics();
  }
}

void structStats::CSVheader( ostream& os, const string& intro ) const {
  os << intro << ",Alpino_status,";
  wordDifficultiesHeader( os );
  compoundHeader( os );
  sentDifficultiesHeader( os );
  infoHeader( os );
  coherenceHeader( os );
  concreetHeader( os );
  persoonlijkheidHeader( os );
  verbHeader( os );
  imperativeHeader( os );
  wordSortHeader( os );
  prepPhraseHeader( os );
  intensHeader( os );
  miscHeader( os );
  os << endl;
}

// Escapes quotes from a string. Found on http://stackoverflow.com/a/1162786
string escape_quotes(const string &before)
{
  string after;

  for (string::size_type i = 0; i < before.length(); ++i) {
    switch (before[i]) {
      case '"':
        after += '"'; // duplicate quotes
      default:
        after += before[i];
    }
  }

  return after;
}

void structStats::toCSV( ostream& os ) const {
  if (!isSentence())
  {
    // For paragraphs and documents, add a sentence and word count.
    os << sentCnt << ",";
    os << wordCnt << ",";
  }
  else
  {
    // For sentences, add the original sentence (quoted)
    os << "\"" << escape_quotes(text) << "\",";
  }
  os << parseFailCnt << ",";
  wordDifficultiesToCSV( os );
  compoundToCSV( os );
  sentDifficultiesToCSV( os );
  informationDensityToCSV( os );
  coherenceToCSV( os );
  concreetToCSV( os );
  persoonlijkheidToCSV( os );
  verbToCSV( os );
  imperativeToCSV( os );
  wordSortToCSV( os );
  prepPhraseToCSV( os );
  intensToCSV( os );
  miscToCSV( os );
  os << endl;
}

void structStats::wordDifficultiesHeader( ostream& os ) const {
  os << "Let_per_wrd,Wrd_per_let,Let_per_wrd_zn,Wrd_per_let_zn,"
     << "Morf_per_wrd,Wrd_per_morf,Morf_per_wrd_zn,Wrd_per_morf_zn,"
     << "Namen_p,Namen_d,"
     << "Freq50_staph,Freq65_Staph,Freq77_Staph,Freq80_Staph,"
     << "Wrd_freq_log,Wrd_freq_zn_log,Lem_freq_log,Lem_freq_zn_log,"
     << "Freq1000,Freq2000,Freq3000,"
     << "Freq5000,Freq10000,Freq20000,"
     << "Freq1000_inhwrd,Freq2000_inhwrd,Freq3000_inhwrd,"
     << "Freq5000_inhwrd,Freq10000_inhwrd,Freq20000_inhwrd,";
}

void structStats::wordDifficultiesToCSV( ostream& os ) const {
  os << std::showpoint
     << proportion( charCnt, wordCnt ) << ","
     << proportion( wordCnt, charCnt ) <<  ","
     << proportion( charCntExNames, (wordCnt-nameCnt) ) << ","
     << proportion( (wordCnt - nameCnt), charCntExNames ) <<  ","
     << proportion( morphCnt, wordCnt ) << ","
     << proportion( wordCnt, morphCnt ) << ","
     << proportion( morphCntExNames, (wordCnt-nameCnt) ) << ","
     << proportion( (wordCnt-nameCnt), morphCntExNames ) << ","

     << proportion( nameCnt, (nameCnt+nounCnt) ) << ","
     << density( nameCnt, wordCnt ) << ",";

  os << proportion( f50Cnt, wordCnt ) << ",";
  os << proportion( f65Cnt, wordCnt ) << ",";
  os << proportion( f77Cnt, wordCnt ) << ",";
  os << proportion( f80Cnt, wordCnt ) << ",";
  os << word_freq_log << ",";
  os << word_freq_log_n << ",";
  os << lemma_freq_log << ",";
  os << lemma_freq_log_n << ",";
  os << proportion( top1000Cnt, wordCnt ) << ",";
  os << proportion( top2000Cnt, wordCnt ) << ",";
  os << proportion( top3000Cnt, wordCnt ) << ",";
  os << proportion( top5000Cnt, wordCnt ) << ",";
  os << proportion( top10000Cnt, wordCnt ) << ",";
  os << proportion( top20000Cnt, wordCnt ) << ",";
  os << proportion( top1000ContentCnt, contentCnt ) << ",";
  os << proportion( top2000ContentCnt, contentCnt ) << ",";
  os << proportion( top3000ContentCnt, contentCnt ) << ",";
  os << proportion( top5000ContentCnt, contentCnt ) << ",";
  os << proportion( top10000ContentCnt, contentCnt ) << ",";
  os << proportion( top20000ContentCnt, contentCnt ) << ",";
}

void structStats::compoundHeader( ostream& os ) const {
  os << "Samenst_d,Samenst_p,Samenst3_d,Samenst3_p,";
  os << "Let_per_wrd_nw,Let_per_wrd_nsam,Let_per_wrd_sam,";
  os << "Let_per_wrd_hfdwrd,Let_per_wrd_satwrd,";
  os << "Let_per_wrd_nw_corr,Let_per_wrd_corr,";
  os << "Wrd_freq_log_nw,Wrd_freq_log_ong_nw,Wrd_freq_log_sam_nw,";
  os << "Wrd_freq_log_hfdwrd,Wrd_freq_log_satwrd,Wrd_freq_log_(hfd_sat),";
  os << "Wrd_freq_log_nw_corr,Wrd_freq_log_corr,";
  os << "Freq1000_nw,Freq5000_nw,Freq20000_nw,";
  os << "Freq1000_nsam_nw,Freq5000_nsam_nw,Freq20000_nsam_nw,";
  os << "Freq1000_sam_nw,Freq5000_sam_nw,Freq20000_sam_nw,";
  os << "Freq1000_hfdwrd_nw,Freq5000_hfdwrd_nw,Freq20000_hfdwrd_nw,";
  os << "Freq1000_satwrd_nw,Freq5000_satwrd_nw,Freq20000_satwrd_nw,";
  os << "Freq1000_nw_corr,Freq5000_nw_corr,Freq20000_nw_corr,";
  os << "Freq1000_corr,Freq5000_corr,Freq20000_corr,";
}

void structStats::compoundToCSV( ostream& os ) const {
  int nonCompoundCnt = nounCnt - compoundCnt;
  os << density(compoundCnt, wordCnt) << ",";
  os << proportion(compoundCnt, nounCnt) << ",";
  os << density(compound3Cnt, wordCnt) << ",";
  os << proportion(compound3Cnt, nounCnt) << ",";
  os << proportion(charCntNoun, nounCnt) << ",";
  os << proportion(charCntNonComp, nonCompoundCnt) << ",";
  os << proportion(charCntComp, compoundCnt) << ",";
  os << proportion(charCntHead, compoundCnt) << ",";
  os << proportion(charCntSat, compoundCnt) << ",";
  os << proportion(charCntNounCorr, nounCnt) << ",";
  os << proportion(charCntCorr, wordCnt) << ",";
  os << proportion(word_freq_log_noun, nounCnt) << ",";
  os << proportion(word_freq_log_non_comp, nonCompoundCnt) << ",";
  os << proportion(word_freq_log_comp, compoundCnt) << ",";
  os << proportion(word_freq_log_head, compoundCnt) << ",";
  os << proportion(word_freq_log_sat, compoundCnt) << ",";
  os << proportion(word_freq_log_head_sat, compoundCnt) << ",";
  os << proportion(word_freq_log_noun_corr, nounCnt) << ",";
  os << proportion(word_freq_log_corr, contentCnt) << ",";
  os << proportion(top1000CntNoun, nounCnt) << ",";
  os << proportion(top5000CntNoun, nounCnt) << ",";
  os << proportion(top20000CntNoun, nounCnt) << ",";
  os << proportion(top1000CntNonComp, nonCompoundCnt) << ",";
  os << proportion(top5000CntNonComp, nonCompoundCnt) << ",";
  os << proportion(top20000CntNonComp, nonCompoundCnt) << ",";
  os << proportion(top1000CntComp, compoundCnt) << ",";
  os << proportion(top5000CntComp, compoundCnt) << ",";
  os << proportion(top20000CntComp, compoundCnt) << ",";
  os << proportion(top1000CntHead, compoundCnt) << ",";
  os << proportion(top5000CntHead, compoundCnt) << ",";
  os << proportion(top20000CntHead, compoundCnt) << ",";
  os << proportion(top1000CntSat, compoundCnt) << ",";
  os << proportion(top5000CntSat, compoundCnt) << ",";
  os << proportion(top20000CntSat, compoundCnt) << ",";
  os << proportion(top1000CntNounCorr, nounCnt) << ",";
  os << proportion(top5000CntNounCorr, nounCnt) << ",";
  os << proportion(top20000CntNounCorr, nounCnt) << ",";
  os << proportion(top1000CntCorr, wordCnt) << ",";
  os << proportion(top5000CntCorr, wordCnt) << ",";
  os << proportion(top20000CntCorr, wordCnt) << ",";
}

void structStats::sentDifficultiesHeader( ostream& os ) const {
  os << "Wrd_per_zin,Wrd_per_dz,Zin_per_wrd,Dzin_per_wrd,"
     << "Wrd_per_nwg,"
     << "Betr_bijzin_per_zin,Bijw_bijzin_per_zin,"
     << "Compl_bijzin_per_zin,Fin_bijzin_per_zin,"
     << "Mv_fin_inbed_per_zin,Infin_compl_per_zin,"
     << "Bijzin_per_zin,Mv_inbed_per_zin,"
     << "Betr_bijzin_los,Bijw_compl_bijzin_los,"
     << "Pv_hzin_per_zin,Pv_bijzin_per_zin,Pv_ww1_per_zin,"
     << "Hzin_conj,Bijzin_conj,Ww1_conj,"
     << "Pv_Alpino_per_zin,"
     << "Pv_Frog_d,Pv_Frog_per_zin,";
  if ( isSentence() ){
    os << "D_level,";
  }
  else {
    os  << "D_level,D_level_gt4_p,";
  }
  os << "Nom_d,Lijdv_d,Lijdv_dz,Ontk_zin_d,Ontk_zin_dz,"
     << "Ontk_morf_d,Ontk_morf_dz,Ontk_tot_d,Ontk_tot_dz,"
     << "Meerv_ontk_d,Meerv_ontk_dz,"
     << "AL_sub_ww,AL_ob_ww,AL_indirob_ww,AL_ww_vzg,"
     << "AL_lidw_znw,AL_vz_znw,AL_ww_wwvc,"
     << "AL_vg_wwbijzin,AL_vg_conj,AL_vg_wwhoofdzin,AL_znw_bijzin,AL_ww_schdw,"
     << "AL_ww_znwpred,AL_ww_bnwpred,AL_ww_bnwbwp,AL_ww_bwbwp,AL_ww_znwbwp,"
     << "AL_gem,AL_max,";
}

void structStats::sentDifficultiesToCSV( ostream& os ) const {
  if ( parseFailCnt > 0 ) {
    os << "NA,";
  }
  else {
    os << proportion( wordCnt, sentCnt ) << ",";
  }

  os << proportion( wordCnt, correctedClauseCnt ) << ",";
  os << proportion( sentCnt, wordCnt )  << ",";
  os << proportion( correctedClauseCnt, wordCnt )  << ",";
  os << proportion( wordCnt, npCnt ) << ",";

  double bijzinCnt = betrCnt + bijwCnt + complCnt;
  if ( parseFailCnt > 0 ) {
    os << "NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,";
  }
  else {
    os << proportion( betrCnt, sentCnt ) << ",";
    os << proportion( bijwCnt, sentCnt ) << ",";
    os << proportion( complCnt, sentCnt ) << ",";
    os << proportion( bijzinCnt, sentCnt ) << ",";
    os << proportion( mvFinInbedCnt, sentCnt ) << ",";
    os << proportion( infinComplCnt, sentCnt ) << ",";
    os << proportion( bijzinCnt + infinComplCnt, sentCnt ) << ",";
    os << proportion( mvInbedCnt, sentCnt ) << ",";
    os << proportion( losBetrCnt, sentCnt ) << ",";
    os << proportion( losBijwCnt, sentCnt ) << ",";
  }

  if ( parseFailCnt > 0 ) {
    os << "NA,NA,NA,NA,NA,NA,NA,";
  }
  else {
    os << proportion( smainCnt, sentCnt ) << ",";
    os << proportion( ssubCnt, sentCnt ) << ",";
    os << proportion( sv1Cnt, sentCnt ) << ",";
    os << proportion( smainCnjCnt, sentCnt ) << ",";
    os << proportion( ssubCnjCnt, sentCnt ) << ",";
    os << proportion( sv1CnjCnt, sentCnt ) << ",";
    os << proportion( clauseCnt, sentCnt ) << ",";
  }

  double frogClauseCnt = pastCnt + presentCnt;
  os << density( frogClauseCnt, wordCnt ) << ",";
  os << proportion( frogClauseCnt, sentCnt ) << ",";

  os << proportion( dLevel, sentCnt ) << ",";
  if ( !isSentence() ){
    os << proportion( dLevel_gt4, sentCnt ) << ",";
  }
  os << density( nominalCnt, wordCnt ) << ",";
  os << density( passiveCnt, wordCnt ) << ",";
  os << proportion( passiveCnt, correctedClauseCnt ) << ",";
  os << density( propNegCnt, wordCnt ) << ",";
  os << proportion( propNegCnt, correctedClauseCnt ) << ",";
  os << density( morphNegCnt, wordCnt ) << ",";
  os << proportion( morphNegCnt, correctedClauseCnt ) << ",";
  os << density( propNegCnt+morphNegCnt, wordCnt ) << ",";
  os << proportion( propNegCnt+morphNegCnt, correctedClauseCnt ) << ",";
  os << density( multiNegCnt, wordCnt ) << ",";
  os << proportion( multiNegCnt, correctedClauseCnt ) << ",";
  os << MMtoString( distances, SUB_VERB ) << ",";
  os << MMtoString( distances, OBJ1_VERB ) << ",";
  os << MMtoString( distances, OBJ2_VERB ) << ",";
  os << MMtoString( distances, VERB_PP ) << ",";
  os << MMtoString( distances, NOUN_DET ) << ",";
  os << MMtoString( distances, PREP_OBJ1 ) << ",";
  os << MMtoString( distances, VERB_VC ) << ",";
  os << MMtoString( distances, COMP_BODY ) << ",";
  os << MMtoString( distances, CRD_CNJ ) << ",";
  os << MMtoString( distances, VERB_COMP ) << ",";
  os << MMtoString( distances, NOUN_VC ) << ",";
  os << MMtoString( distances, VERB_SVP ) << ",";
  os << MMtoString( distances, VERB_PREDC_N ) << ",";
  os << MMtoString( distances, VERB_PREDC_A ) << ",";
  os << MMtoString( distances, VERB_MOD_A ) << ",";
  os << MMtoString( distances, VERB_MOD_BW ) << ",";
  os << MMtoString( distances, VERB_NOUN ) << ",";
  os << toMString( al_gem ) << ",";
  os << toMString( al_max ) << ",";
}

void structStats::infoHeader( ostream& os ) const {
  os << "Bijw_bep_d,Bijw_bep_dz,"
     << "Bijw_bep_alg_d,Bijw_bep_alg_dz,"
     << "Bijv_bep_d,Bijv_bep_dz,"
     << "Attr_bijv_nw_d,Attr_bijv_nw_dz,"
     << "Ov_bijv_bep_d,Ov_bijv_bep_dz,"
     << "TTR_wrd,MTLD_wrd,TTR_lem,MTLD_lem,"
     << "TTR_namen,MTLD_namen,TTR_inhwrd,MTLD_inhwrd,"
     << "Inhwrd_d,Inhwrd_dz,"
     << "Zeldz_index,"
     << "Vnw_ref_d,Vnw_ref_dz,"
     << "Arg_over_vzin_d,Arg_over_vzin_dz,Lem_over_vzin_d,Lem_over_vzin_dz,"
     << "Arg_over_buf_d,Arg_over_buf_dz,Lem_over_buf_d,Lem_over_buf_dz,"
     << "Onbep_nwg_p,Onbep_nwg_dz,";
}

void structStats::informationDensityToCSV( ostream& os ) const {
  os << density( vcModCnt, wordCnt ) << ",";
  os << proportion( vcModCnt, correctedClauseCnt ) << ",";
  os << density( vcModSingleCnt, wordCnt ) << ",";
  os << proportion( vcModSingleCnt, correctedClauseCnt ) << ",";
  os << density( npModCnt, wordCnt ) << ",";
  os << proportion( npModCnt, correctedClauseCnt ) << ",";
  os << density( adjNpModCnt, wordCnt ) << ",";
  os << proportion( adjNpModCnt, correctedClauseCnt ) << ",";
  os << density( npModCnt-adjNpModCnt, wordCnt ) << ",";
  os << proportion( npModCnt-adjNpModCnt, correctedClauseCnt ) << ",";

  os << proportion( unique_words.size(), wordCnt ) << ",";
  os << word_mtld << ",";
  os << proportion( unique_lemmas.size(), wordCnt ) << ",";
  os << lemma_mtld << ",";
  os << proportion( unique_names.size(), nameCnt ) << ",";
  os << name_mtld << ",";
  os << proportion( unique_contents.size(), contentCnt ) << ",";
  os << content_mtld << ",";
  os << density( contentCnt, wordCnt ) << ",";
  os << proportion( contentCnt, correctedClauseCnt ) << ",";
  os << rarity( settings.rarityLevel ) << ",";
  os << density( pronRefCnt, wordCnt ) << ",";
  os << proportion( pronRefCnt, correctedClauseCnt ) << ",";
  if ( isSentence() ){
    if ( index == 0 ){
      os << "NA,NA,"
	 << "NA,NA,";
    }
    else {
      os << density( wordOverlapCnt, wordCnt ) << ",NA,"
	 << density( lemmaOverlapCnt, wordCnt ) << ",NA,";
    }
  }
  else {
    os << density( wordOverlapCnt, wordCnt ) << ",";
    os << proportion( wordOverlapCnt, correctedClauseCnt ) << ",";
    os << density( lemmaOverlapCnt, wordCnt ) << ",";
    os << proportion( lemmaOverlapCnt, correctedClauseCnt ) << ",";
  }
  if ( !isDocument() ){
    os << "NA,NA,NA,NA,";
  }
  else {
    os << density( word_overlapCnt(), wordCnt - settings.overlapSize ) << ",";
    os << proportion( word_overlapCnt(), correctedClauseCnt ) << ",";
    os << density( lemma_overlapCnt(), wordCnt  - settings.overlapSize ) << ",";
    os << proportion( lemma_overlapCnt(), correctedClauseCnt ) << ",";
  }
  os << proportion( indefNpCnt, npCnt ) << ",";
  os << proportion( indefNpCnt, correctedClauseCnt ) << ",";
}

void structStats::coherenceHeader( ostream& os ) const {
  os << "Conn_temp_d,Conn_temp_dz,Conn_temp_TTR,Conn_temp_MTLD,"
     << "Conn_reeks_wg_d,Conn_reeks_wg_dz,Conn_reeks_wg_TTR,Conn_reeks_wg_MTLD,"
     << "Conn_reeks_zin_d,Conn_reeks_zin_dz,Conn_reeks_zin_TTR,Conn_reeks_zin_MTLD,"
     << "Conn_contr_d,Conn_contr_dz,Conn_contr_TTR,Conn_contr_MTLD,"
     << "Conn_comp_d,Conn_comp_dz,Conn_comp_TTR,Conn_comp_MTLD,"
     << "Conn_caus_d,Conn_caus_dz,Conn_caus_TTR,Conn_caus_MTLD,"
     << "Causaal_d,Ruimte_d,Tijd_d,Emotie_d,"
     << "Causaal_TTR,Causaal_MTLD,"
     << "Ruimte_TTR,Ruimte_MTLD,"
     << "Tijd_TTR,Tijd_MTLD,"
     << "Emotie_TTR,Emotie_MTLD,";
}

void structStats::coherenceToCSV( ostream& os ) const {
  os << density( tempConnCnt, wordCnt ) << ",";
  os << proportion( tempConnCnt, correctedClauseCnt ) << ",";
  os << proportion( unique_temp_conn.size(), tempConnCnt ) << ",";
  os << temp_conn_mtld << ",";
  os << density( opsomWgConnCnt, wordCnt ) << ",";
  os << proportion( opsomWgConnCnt, correctedClauseCnt ) << ",";
  os << proportion( unique_reeks_wg_conn.size(), opsomWgConnCnt ) << ",";
  os << reeks_zin_conn_mtld << ",";
  os << density( opsomZinConnCnt, wordCnt ) << ",";
  os << proportion( opsomZinConnCnt, correctedClauseCnt ) << ",";
  os << proportion( unique_reeks_zin_conn.size(), opsomZinConnCnt ) << ",";
  os << reeks_zin_conn_mtld << ",";
  os << density( contrastConnCnt, wordCnt ) << ",";
  os << proportion( contrastConnCnt, correctedClauseCnt ) << ",";
  os << proportion( unique_contr_conn.size(), contrastConnCnt ) << ",";
  os << contr_conn_mtld << ",";
  os << density( compConnCnt, wordCnt ) << ",";
  os << proportion( compConnCnt, correctedClauseCnt ) << ",";
  os << proportion( unique_comp_conn.size(), compConnCnt ) << ",";
  os << comp_conn_mtld << ",";
  os << density( causeConnCnt, wordCnt ) << ",";
  os << proportion( causeConnCnt, correctedClauseCnt ) << ",";
  os << proportion( unique_cause_conn.size(), causeConnCnt ) << ",";
  os << cause_conn_mtld << ",";
  os << density( causeSitCnt, wordCnt ) << ",";
  os << density( spaceSitCnt, wordCnt ) << ",";
  os << density( timeSitCnt, wordCnt ) << ",";
  os << density( emoSitCnt, wordCnt ) << ",";
  os << proportion( unique_cause_sits.size(), causeSitCnt ) << ",";
  os << cause_sit_mtld << ",";
  os << proportion( unique_ruimte_sits.size(), spaceSitCnt ) << ",";
  os << ruimte_sit_mtld << ",";
  os << proportion( unique_tijd_sits.size(), timeSitCnt ) << ",";
  os << tijd_sit_mtld << ",";
  os << proportion( unique_emotion_sits.size(), emoSitCnt ) << ",";
  os << emotion_sit_mtld << ",";
}

void structStats::concreetHeader( ostream& os ) const {
  os << "Conc_nw_strikt_p,Conc_nw_strikt_d,";
  os << "Conc_nw_ruim_p,Conc_nw_ruim_d,";
  os << "Pers_nw_p,Pers_nw_d,";
  os << "PlantDier_nw_p,PlantDier_nw_d,";
  os << "Gebr_vw_nw_p,Gebr_vw_nw_d,"; // 20141003: Features renamed
  os << "Subst_conc_nw_p,Subst_conc_nw_d,"; // 20150508: Features renamed
  os << "Voed_verz_nw_p,Voed_verz_nw_d,"; // 20150508: Features added
  os << "Concr_ov_nw_p,Concr_ov_nw_d,";
  os << "Gebeuren_conc_nw_p,Gebeuren_conc_nw_d,"; // 20141031: Features split
  os << "Plaats_nw_p,Plaats_nw_d,";
  os << "Tijd_nw_p,Tijd_nw_d,";
  os << "Maat_nw_p,Maat_nw_d,";
  os << "Subst_abstr_nw_p,Subst_abstr_nw_d,"; // 20150508: Features added
  os << "Gebeuren_abstr_nw_p,Gebeuren_abstr_nw_d,"; // 20141031: Features split
  os << "Organisatie_nw_p,Organisatie_nw_d,";
  os << "Ov_abstr_nw_p,Ov_abstr_nw_d,"; // 20150508: Features renamed
  os << "Undefined_nw_p,";
  os << "Gedekte_nw_p,";
  os << "Alg_nw_d,Alg_nw_p,";
  os << "Alg_nw_afz_sit_d,Alg_nw_afz_sit_p,";
  os << "Alg_nw_rel_sit_d,Alg_nw_rel_sit_p,";
  os << "Alg_nw_hand_d,Alg_nw_hand_p,";
  os << "Alg_nw_kenn_d,Alg_nw_kenn_p,";
  os << "Alg_nw_disc_caus_d,Alg_nw_disc_caus_p,";
  os << "Alg_nw_ontw_d,Alg_nw_ontw_p,";
  os << "Waarn_mens_bvnw_p,Waarn_mens_bvnw_d,";
  os << "Emosoc_bvnw_p,Emosoc_bvnw_d,";
  os << "Waarn_nmens_bvnw_p,Waarn_nmens_bvnw_d,";
  os << "Vorm_omvang_bvnw_p,Vorm_omvang_bvnw_d,";
  os << "Kleur_bvnw_p,Kleur_bvnw_d,";
  os << "Stof_bvnw_p,Stof_bvnw_d,";
  os << "Geluid_bvnw_p,Geluid_bvnw_d,";
  os << "Waarn_nmens_ov_bvnw_p,Waarn_nmens_ov_bvnw_d,";
  os << "Technisch_bvnw_p,Technisch_bvnw_d,";
  os << "Tijd_bvnw_p,Tijd_bvnw_d,";
  os << "Plaats_bvnw_p,Plaats_bvnw_d,";
  os << "Spec_positief_bvnw_p,Spec_positief_bvnw_d,";
  os << "Spec_negatief_bvnw_p,Spec_negatief_bvnw_d,";
  os << "Alg_positief_bvnw_p,Alg_positief_bvnw_d,";
  os << "Alg_negatief_bvnw_p,Alg_negatief_bvnw_d,";
  os << "Alg_ev_zr_bvnw_p,Alg_ev_zr_bvnw_d,"; // 20150316: Features added
  os << "Ep_positief_bvnw_p,Ep_positief_bvnw_d,";
  os << "Ep_negatief_bvnw_p,Ep_negatief_bvnw_d,";
  os << "Ov_abstr_bvnw_p,Ov_abstr_bvnw_d,"; // 20151020: Features renamed
  os << "Spec_ev_bvnw_p,Spec_ev_bvnw_d,"; // 20150316: Features renamed
  os << "Alg_ev_bvnw_p,Alg_ev_bvnw_d,"; // 20150316: Features renamed
  os << "Ep_ev_bvnw_p,Ep_ev_bvnw_d,"; // 20150316: Features renamed
  os << "Conc_bvnw_strikt_p,Conc_bvnw_strikt_d,";
  os << "Conc_bvnw_ruim_p,Conc_bvnw_ruim_d,";
  os << "Subj_bvnw_p,Subj_bvnw_d,";
  os << "Undefined_bvnw_p,";
  os << "Gelabeld_bvnw_p,";
  os << "Gedekte_bvnw_p,";
  os << "Conc_ww_p,Conc_ww_d,";
  os << "Abstr_ww_p,Abstr_ww_d,";
  os << "Undefined_ww_p,";
  os << "Gedekte_ww_p,";
  os << "Alg_ww_d,Alg_ww_p,";
  os << "Alg_ww_afz_sit_d,Alg_ww_afz_sit_p,";
  os << "Alg_ww_rel_sit_d,Alg_ww_rel_sit_p,";
  os << "Alg_ww_hand_d,Alg_ww_hand_p,";
  os << "Alg_ww_kenn_d,Alg_ww_kenn_p,";
  os << "Alg_ww_disc_caus_d,Alg_ww_disc_caus_p,";
  os << "Alg_ww_ontw_d,Alg_ww_ontw_p,";
  os << "Conc_tot_p,Conc_tot_d,";
  os << "Alg_bijw_d,Alg_bijw_p,";
  os << "Spec_bijw_d,Spec_bijw_p,";
  os << "Gedekte_bw_p,";
}

void structStats::concreetToCSV( ostream& os ) const {
  int coveredNouns = nounCnt+nameCnt-uncoveredNounCnt;
  os << proportion( strictNounCnt, coveredNouns ) << ",";
  os << density( strictNounCnt, wordCnt ) << ",";
  os << proportion( broadNounCnt, coveredNouns ) << ",";
  os << density( broadNounCnt, wordCnt ) << ",";
  os << proportion( humanCnt, coveredNouns ) << ",";
  os << density( humanCnt, wordCnt ) << ",";
  os << proportion( nonHumanCnt, coveredNouns ) << ",";
  os << density( nonHumanCnt, wordCnt ) << ",";
  os << proportion( artefactCnt, coveredNouns ) << ",";
  os << density( artefactCnt, wordCnt ) << ",";
  os << proportion( substanceConcCnt, coveredNouns ) << ",";
  os << density( substanceConcCnt, wordCnt ) << ",";
  os << proportion( foodcareCnt, coveredNouns ) << ",";
  os << density( foodcareCnt, wordCnt ) << ",";
  os << proportion( concrotherCnt, coveredNouns ) << ",";
  os << density( concrotherCnt, wordCnt ) << ",";
  os << proportion( dynamicConcCnt, coveredNouns ) << ",";
  os << density( dynamicConcCnt, wordCnt ) << ",";
  os << proportion( placeCnt, coveredNouns ) << ",";
  os << density( placeCnt, wordCnt ) << ",";
  os << proportion( timeCnt, coveredNouns ) << ",";
  os << density( timeCnt, wordCnt ) << ",";
  os << proportion( measureCnt, coveredNouns ) << ",";
  os << density( measureCnt, wordCnt ) << ",";
  os << proportion( substanceAbstrCnt, coveredNouns ) << ",";
  os << density( substanceAbstrCnt, wordCnt ) << ",";
  os << proportion( dynamicAbstrCnt, coveredNouns ) << ",";
  os << density( dynamicAbstrCnt, wordCnt ) << ",";
  os << proportion( institutCnt, coveredNouns ) << ",";
  os << density( institutCnt, wordCnt ) << ",";
  os << proportion( nonDynamicCnt, coveredNouns ) << ",";
  os << density( nonDynamicCnt, wordCnt ) << ",";
  os << proportion( undefinedNounCnt, coveredNouns ) << ",";
  os << proportion( coveredNouns, nounCnt + nameCnt ) << ",";

  os << density( generalNounCnt, wordCnt ) << ",";
  os << proportion( generalNounCnt, coveredNouns ) << ",";
  os << density( generalNounSepCnt, wordCnt ) << ",";
  os << proportion( generalNounSepCnt, coveredNouns ) << ",";
  os << density( generalNounRelCnt, wordCnt ) << ",";
  os << proportion( generalNounRelCnt, coveredNouns ) << ",";
  os << density( generalNounActCnt, wordCnt ) << ",";
  os << proportion( generalNounActCnt, coveredNouns ) << ",";
  os << density( generalNounKnowCnt, wordCnt ) << ",";
  os << proportion( generalNounKnowCnt, coveredNouns ) << ",";
  os << density( generalNounDiscCnt, wordCnt ) << ",";
  os << proportion( generalNounDiscCnt, coveredNouns ) << ",";
  os << density( generalNounDeveCnt, wordCnt ) << ",";
  os << proportion( generalNounDeveCnt, coveredNouns ) << ",";

  int coveredAdj = adjCnt-uncoveredAdjCnt;
  os << proportion( humanAdjCnt, coveredAdj ) << ",";
  os << density( humanAdjCnt,wordCnt ) << ",";
  os << proportion( emoAdjCnt, coveredAdj ) << ",";
  os << density( emoAdjCnt,wordCnt ) << ",";
  os << proportion( nonhumanAdjCnt, coveredAdj ) << ",";
  os << density( nonhumanAdjCnt,wordCnt ) << ",";
  os << proportion( shapeAdjCnt, coveredAdj ) << ",";
  os << density( shapeAdjCnt,wordCnt ) << ",";
  os << proportion( colorAdjCnt, coveredAdj ) << ",";
  os << density( colorAdjCnt,wordCnt ) << ",";
  os << proportion( matterAdjCnt, coveredAdj ) << ",";
  os << density( matterAdjCnt,wordCnt ) << ",";
  os << proportion( soundAdjCnt, coveredAdj ) << ",";
  os << density( soundAdjCnt,wordCnt ) << ",";
  os << proportion( nonhumanOtherAdjCnt, coveredAdj ) << ",";
  os << density( nonhumanOtherAdjCnt,wordCnt ) << ",";
  os << proportion( techAdjCnt, coveredAdj ) << ",";
  os << density( techAdjCnt,wordCnt ) << ",";
  os << proportion( timeAdjCnt, coveredAdj ) << ",";
  os << density( timeAdjCnt,wordCnt ) << ",";
  os << proportion( placeAdjCnt, coveredAdj ) << ",";
  os << density( placeAdjCnt,wordCnt ) << ",";
  os << proportion( specPosAdjCnt, coveredAdj ) << ",";
  os << density( specPosAdjCnt,wordCnt ) << ",";
  os << proportion( specNegAdjCnt, coveredAdj ) << ",";
  os << density( specNegAdjCnt,wordCnt ) << ",";
  os << proportion( posAdjCnt, coveredAdj ) << ",";
  os << density( posAdjCnt,wordCnt ) << ",";
  os << proportion( negAdjCnt, coveredAdj ) << ",";
  os << density( negAdjCnt,wordCnt ) << ",";
  os << proportion( evaluativeAdjCnt, coveredAdj ) << ",";
  os << density( evaluativeAdjCnt,wordCnt ) << ",";
  os << proportion( epiPosAdjCnt, coveredAdj ) << ",";
  os << density( epiPosAdjCnt,wordCnt ) << ",";
  os << proportion( epiNegAdjCnt, coveredAdj ) << ",";
  os << density( epiNegAdjCnt,wordCnt ) << ",";
  os << proportion( abstractAdjCnt, coveredAdj ) << ",";
  os << density( abstractAdjCnt,wordCnt ) << ",";
  os << proportion( specPosAdjCnt + specNegAdjCnt, coveredAdj ) << ",";
  os << density( specPosAdjCnt + specNegAdjCnt, wordCnt ) << ",";
  os << proportion( posAdjCnt + negAdjCnt + evaluativeAdjCnt, coveredAdj ) << ",";
  os << density( posAdjCnt + negAdjCnt + evaluativeAdjCnt, wordCnt ) << ",";
  os << proportion( epiPosAdjCnt + epiNegAdjCnt, coveredAdj ) << ",";
  os << density( epiPosAdjCnt + epiNegAdjCnt ,wordCnt ) << ",";
  os << proportion( strictAdjCnt, coveredAdj ) << ",";
  os << density( strictAdjCnt, wordCnt ) << ",";
  os << proportion( broadAdjCnt, coveredAdj ) << ",";
  os << density( broadAdjCnt, wordCnt ) << ",";
  os << proportion( subjectiveAdjCnt ,coveredAdj ) << ",";
  os << density( subjectiveAdjCnt, wordCnt ) << ",";
  os << proportion( undefinedAdjCnt, coveredAdj ) << ",";
  os << proportion( coveredAdj - undefinedAdjCnt ,coveredAdj ) << ",";
  os << proportion( coveredAdj ,adjCnt ) << ",";

  int coveredVerbs = verbCnt - uncoveredVerbCnt;
  os << proportion( concreteWwCnt, coveredVerbs ) << ",";
  os << density( concreteWwCnt, wordCnt ) << ",";
  os << proportion( abstractWwCnt, coveredVerbs ) << ",";
  os << density( abstractWwCnt, wordCnt ) << ",";
  os << proportion( undefinedWwCnt, coveredVerbs ) << ",";
  os << proportion( coveredVerbs, verbCnt ) << ",";

  os << density( generalVerbCnt, wordCnt ) << ",";
  os << proportion( generalVerbCnt, coveredVerbs ) << ",";
  os << density( generalVerbSepCnt, wordCnt ) << ",";
  os << proportion( generalVerbSepCnt, coveredVerbs ) << ",";
  os << density( generalVerbRelCnt, wordCnt ) << ",";
  os << proportion( generalVerbRelCnt, coveredVerbs ) << ",";
  os << density( generalVerbActCnt, wordCnt ) << ",";
  os << proportion( generalVerbActCnt, coveredVerbs ) << ",";
  os << density( generalVerbKnowCnt, wordCnt ) << ",";
  os << proportion( generalVerbKnowCnt, coveredVerbs ) << ",";
  os << density( generalVerbDiscCnt, wordCnt ) << ",";
  os << proportion( generalVerbDiscCnt, coveredVerbs ) << ",";
  os << density( generalVerbDeveCnt, wordCnt ) << ",";
  os << proportion( generalVerbDeveCnt, coveredVerbs ) << ",";

  int totalCovered = coveredNouns + coveredAdj + coveredVerbs;
  int totalCnt = strictNounCnt + strictAdjCnt + concreteWwCnt;
  os << proportion( totalCnt, totalCovered ) << ",";
  os << density( totalCnt, wordCnt ) << ",";

  int coveredAdverbs = generalAdverbCnt + specificAdverbCnt;
  os << density( generalAdverbCnt, wordCnt ) << ",";
  os << proportion( generalAdverbCnt, coveredAdverbs ) << ",";
  os << density( specificAdverbCnt, wordCnt ) << ",";
  os << proportion( specificAdverbCnt, coveredAdverbs ) << ",";
  os << proportion( coveredAdverbs, bwCnt ) << ",";
}

void structStats::persoonlijkheidHeader( ostream& os ) const {
  os << "Pers_ref_d,Pers_vnw1_d,Pers_vnw2_d,Pers_vnw3_d,Pers_vnw_d,"
     << "Pers_namen_p, Pers_namen_p2, Pers_namen_d, Plaatsnamen_d,"
     << "Org_namen_d, Prod_namen_d, Event_namen_d,";
}

void structStats::persoonlijkheidToCSV( ostream& os ) const {
  os << density( persRefCnt, wordCnt ) << ",";
  os << density( pron1Cnt, wordCnt ) << ",";
  os << density( pron2Cnt, wordCnt ) << ",";
  os << density( pron3Cnt, wordCnt ) << ",";
  os << density( pron1Cnt+pron2Cnt+pron3Cnt, wordCnt ) << ",";

  int val = at( ners, NER::PER_B );
  os << proportion( val, nerCnt ) << ",";
  os << proportion( val, nounCnt + nameCnt ) << ",";
  os << density( val, wordCnt ) << ",";
  val = at( ners, NER::LOC_B );
  os << density( val, wordCnt ) << ",";
  val = at( ners, NER::ORG_B );
  os << density( val, wordCnt ) << ",";
  val = at( ners, NER::PRO_B );
  os << density( val, wordCnt ) << ",";
  val = at( ners, NER::EVE_B );
  os << density( val, wordCnt ) << ",";
}

void structStats::verbHeader( ostream& os ) const {
  os << "Actieww_p,Actieww_d,Toestww_p,Toestww_d,"
     << "Procesww_p,Procesww_d,Undefined_ATP_ww_p,"
     << "Ww_tt_p,Ww_tt_dz,Ww_mod_d_,Ww_mod_dz,"
     << "Huww_tijd_d,Huww_tijd_dz,Koppelww_d,Koppelww_dz,"
     << "Infin_bv_d,Infin_bv_dz,"
     << "Infin_nw_d,Infin_nw_dz,"
     << "Infin_vrij_d,Infin_vrij_dz,"
     << "Vd_bv_d,Vd_bv_dz,"
     << "Vd_nw_d,Vd_nw_dz,"
     << "Vd_vrij_d,Vd_vrij_dz,"
     << "Ovd_bv_d,Ovd_bv_dz,"
     << "Ovd_nw_d,Ovd_nw_dz,"
     << "Ovd_vrij_d,Ovd_vrij_dz,";
}

void structStats::verbToCSV( ostream& os ) const {
  os << proportion( actionCnt, verbCnt ) << ",";
  os << density( actionCnt, wordCnt) << ",";
  os << proportion( stateCnt, verbCnt ) << ",";
  os << density( stateCnt, wordCnt ) << ",";
  os << proportion( processCnt, verbCnt ) << ",";
  os << density( processCnt, wordCnt ) << ",";
  os << proportion( undefinedATPCnt, verbCnt - uncoveredVerbCnt ) << ",";

  os << density( presentCnt, wordCnt ) << ",";
  os << proportion( presentCnt, correctedClauseCnt ) << ",";
  os << density( modalCnt, wordCnt ) << ",";
  os << proportion( modalCnt, correctedClauseCnt ) << ",";
  os << density( timeVCnt, wordCnt ) << ",";
  os << proportion( timeVCnt, correctedClauseCnt ) << ",";
  os << density( koppelCnt, wordCnt ) << ",";
  os << proportion( koppelCnt, correctedClauseCnt ) << ",";

  os << density( infBvCnt, wordCnt ) << ",";
  os << proportion( infBvCnt, correctedClauseCnt ) << ",";
  os << density( infNwCnt, wordCnt ) << ",";
  os << proportion( infNwCnt, correctedClauseCnt ) << ",";
  os << density( infVrijCnt, wordCnt ) << ",";
  os << proportion( infVrijCnt, correctedClauseCnt ) << ",";

  os << density( vdBvCnt, wordCnt ) << ",";
  os << proportion( vdBvCnt, correctedClauseCnt ) << ",";
  os << density( vdNwCnt, wordCnt ) << ",";
  os << proportion( vdNwCnt, correctedClauseCnt ) << ",";
  os << density( vdVrijCnt, wordCnt ) << ",";
  os << proportion( vdVrijCnt, correctedClauseCnt ) << ",";

  os << density( odBvCnt, wordCnt ) << ",";
  os << proportion( odBvCnt, correctedClauseCnt ) << ",";
  os << density( odNwCnt, wordCnt ) << ",";
  os << proportion( odNwCnt, correctedClauseCnt ) << ",";
  os << density( odVrijCnt, wordCnt ) << ",";
  os << proportion( odVrijCnt, correctedClauseCnt ) << ",";
}

void structStats::imperativeHeader( ostream& os ) const {
  os << "Imp_ellips_p,Imp_ellips_d," // 20141003: Features renamed
     << "Vragen_p,Vragen_d,";
}

void structStats::imperativeToCSV( ostream& os ) const {
  os << proportion( impCnt, sentCnt ) << ",";
  os << density( impCnt, wordCnt ) << ",";
  os << proportion( questCnt, sentCnt ) << ",";
  os << density( questCnt, wordCnt ) << ",";
}

void structStats::wordSortHeader( ostream& os ) const {
  os << "Bvnw_d,Vg_d,Vnw_d,Lidw_d,Vz_d,Bijw_d,Tw_d,Nw_d,Ww_d,Tuss_d,Spec_d,"
     << "Interp_d,"
     << "Afk_d,Afk_gen_d,Afk_int_d,Afk_jur_d,Afk_med_d,"
     << "Afk_ond_d,Afk_pol_d,Afk_ov_d,Afk_zorg_d,";
}

void structStats::wordSortToCSV( ostream& os ) const {
  os << density(adjCnt, wordCnt ) << ","
     << density(vgCnt, wordCnt ) << ","
     << density(vnwCnt, wordCnt ) << ","
     << density(lidCnt, wordCnt ) << ","
     << density(vzCnt, wordCnt ) << ","
     << density(bwCnt, wordCnt ) << ","
     << density(twCnt, wordCnt ) << ","
     << density(nounCnt, wordCnt ) << ","
     << density(verbCnt, wordCnt ) << ","
     << density(tswCnt, wordCnt ) << ","
     << density(specCnt, wordCnt ) << ","
     << density(letCnt, wordCnt ) << ",";
  int pola = at( afks, Afk::OVERHEID_A );
  int jura = at( afks, Afk::JURIDISCH_A );
  int onda = at( afks, Afk::ONDERWIJS_A );
  int meda = at( afks, Afk::MEDIA_A );
  int gena = at( afks, Afk::GENERIEK_A );
  int ova = at( afks, Afk::OVERIGE_A );
  int zorga = at( afks, Afk::ZORG_A );
  int inta = at( afks, Afk::INTERNATIONAAL_A );
  os << density( gena+inta+jura+meda+onda+pola+ova+zorga, wordCnt ) << ","
     << density( gena, wordCnt ) << ","
     << density( inta, wordCnt ) << ","
     << density( jura, wordCnt ) << ","
     << density( meda, wordCnt ) << ","
     << density( onda, wordCnt ) << ","
     << density(  pola, wordCnt ) << ","
     << density( ova, wordCnt ) << ","
     << density( zorga, wordCnt ) << ",";
}

void structStats::prepPhraseHeader( ostream& os ) const {
  os << "Vzu_d,Vzu_dz,Arch_d,";
}

void structStats::prepPhraseToCSV( ostream& os ) const {
  os << density( prepExprCnt, wordCnt ) << ",";
  os << proportion( prepExprCnt, correctedClauseCnt ) << ",";
  os << density( archaicsCnt, wordCnt ) << ",";
}

void structStats::intensHeader( ostream& os ) const {
  os << "Int_d,Int_bvnw_d,Int_bvbw_d,";
  os << "Int_bw_d,Int_combi_d,Int_nw_d,";
  os << "Int_tuss_d,Int_ww_d,";
}

void structStats::intensToCSV( ostream& os ) const {
  os << density( intensCnt, wordCnt ) << ",";
  os << density( intensBvnwCnt, wordCnt ) << ",";
  os << density( intensBvbwCnt, wordCnt ) << ",";
  os << density( intensBwCnt, wordCnt ) << ",";
  os << density( intensCombiCnt, wordCnt ) << ",";
  os << density( intensNwCnt, wordCnt ) << ",";
  os << density( intensTussCnt, wordCnt ) << ",";
  os << density( intensWwCnt, wordCnt ) << ",";
}

void structStats::miscHeader( ostream& os ) const {
  os << "Log_prob,Entropie,Perplexiteit,";
}

void structStats::miscToCSV( ostream& os ) const {
  os << proportion( avg_prob10, sentCnt ) << ",";
  os << proportion( entropy, sentCnt ) << ",";
  os << proportion( perplexity, sentCnt ) << ",";
}

// #define DEBUG_LSA

void structStats::resolveLSA( const map<string,double>& LSA_dists ){
  if ( sv.size() < 1 )
    return;

  calculate_LSA_summary();
  double suc = 0;
  double net = 0;
  double ctx = 0;
  size_t node_count = 0;
  for ( size_t i=0; i < sv.size()-1; ++i ){
    double context = 0.0;
    for ( size_t j=0; j < sv.size(); ++j ){
      if ( j == i )
	continue;
      string word1 = sv[i]->id;
      string word2 = sv[j]->id;
      string call = word1 + "<==>" + word2;
      map<string,double>::const_iterator it = LSA_dists.find(call);
      if ( it != LSA_dists.end() ){
	context += it->second;
      }
    }
    sv[i]->setLSAcontext(context/(sv.size()-1));
    ctx += context;
    for ( size_t j=i+1; j < sv.size(); ++j ){
      ++node_count;
      string word1 = sv[i]->id;
      string word2 = sv[j]->id;
      string call = word1 + "<==>" + word2;
      double result = 0;
      map<string,double>::const_iterator it = LSA_dists.find(call);
      if ( it != LSA_dists.end() ){
	result = it->second;
	if ( j == i+1 ){
	  sv[i]->setLSAsuc(result);
	  suc += result;
	}
	net += result;
      }
#ifdef DEBUG_LSA
      cerr << "LSA: " << category << " lookup '" << call << "' ==> " << result << endl;
#endif
    }
  }
#ifdef DEBUG_LSA
  cerr << "LSA-" << category << "-SUC sum = " << suc << ", size = " << sv.size() << endl;
  cerr << "LSA-" << category << "-NET sum = " << net << ", node count = " << node_count << endl;
  cerr << "LSA-" << category << "-CTX sum = " << ctx << ", size = " << sv.size() << endl;
#endif
  suc = suc/sv.size();
  net = net/node_count;
  ctx = ctx/sv.size();
#ifdef DEBUG_LSA
  cerr << "LSA-" << category << "-SUC result = " << suc << endl;
  cerr << "LSA-" << category << "-NET result = " << net << endl;
  cerr << "LSA-" << category << "-CTX result = " << ctx << endl;
#endif
  setLSAvalues( suc, net, ctx );
}

void structStats::calculate_LSA_summary(){
  double word_suc=0;
  double word_net=0;
  double sent_suc=0;
  double sent_net=0;
  double sent_ctx=0;
  double par_suc=0;
  double par_net=0;
  double par_ctx=0;
  size_t size = sv.size();
  for ( size_t i=0; i != size; ++i ){
    structStats *ps = dynamic_cast<structStats*>(sv[i]);
    if ( !std::isnan(ps->lsa_word_suc) ){
      word_suc += ps->lsa_word_suc;
    }
    if ( !std::isnan(ps->lsa_word_net) ){
      word_net += ps->lsa_word_net;
    }
    if ( !std::isnan(ps->lsa_sent_suc) ){
      sent_suc += ps->lsa_sent_suc;
    }
    if ( !std::isnan(ps->lsa_sent_net) ){
      sent_net += ps->lsa_sent_net;
    }
    if ( !std::isnan(ps->lsa_sent_ctx) ){
      sent_ctx += ps->lsa_sent_ctx;
    }
    if ( !std::isnan(ps->lsa_par_suc) ){
      par_suc += ps->lsa_par_suc;
    }
    if ( !std::isnan(ps->lsa_par_net) ){
      par_net += ps->lsa_par_net;
    }
    if ( !std::isnan(ps->lsa_par_ctx) ){
      par_ctx += ps->lsa_par_ctx;
    }
  }
#ifdef DEBUG_LSA
  cerr << category << " calculate summary, for " << size << " items" << endl;
  cerr << "word_suc = " << word_suc << endl;
  cerr << "word_net = " << word_net << endl;
  cerr << "sent_suc = " << sent_suc << endl;
  cerr << "sent_net = " << sent_net << endl;
  cerr << "sent_ctx = " << sent_ctx << endl;
  cerr << "par_suc = " << par_suc << endl;
  cerr << "par_net = " << par_net << endl;
  cerr << "par_ctx = " << par_ctx << endl;
#endif
  if ( word_suc > 0 ){
    lsa_word_suc = word_suc/size;
  }
  if ( word_net > 0 ){
    lsa_word_net = word_net/size;
  }
  if ( sent_suc > 0 ){
    lsa_sent_suc = sent_suc/size;
  }
  if ( sent_net > 0 ){
    lsa_sent_net = sent_net/size;
  }
  if ( sent_ctx > 0 ){
    lsa_sent_ctx = sent_ctx/size;
  }
  if ( par_suc > 0 ){
    lsa_par_suc = par_suc/size;
  }
  if ( par_net > 0 ){
    lsa_par_net = par_net/size;
  }
  if ( par_ctx > 0 ){
    lsa_par_ctx = par_ctx/size;
  }
}

//#define DEBUG_MTLD

double calculate_mtld( const vector<string>& v ){
  if ( v.size() == 0 ){
    return 0.0;
  }
  int token_count = 0;
  set<string> unique_tokens;
  double token_factor = 0.0;
  double token_ttr = 1.0;
  for ( size_t i=0; i < v.size(); ++i ){
    ++token_count;
    unique_tokens.insert(v[i]);
    token_ttr = unique_tokens.size() / double(token_count);
#ifdef DEBUG_MTLD
    cerr << v[i] << "\t [" << unique_tokens.size() << "/"
	 << token_count << "] >> ttr " << token_ttr << endl;
#endif
    if ( token_ttr <= settings.mtld_threshold ){
#ifdef KOIZUMI
      if ( token_count >=10 ){
	token_factor += 1.0;
      }
#else
      token_factor += 1.0;
#endif
      token_count = 0;
      token_ttr = 1.0;
      unique_tokens.clear();
#ifdef DEBUG_MTLD
      cerr <<"\treset: token_factor = " << token_factor << endl << endl;
#endif
    }
    else if ( i == v.size()-1 ){
#ifdef DEBUG_MTLD
      cerr << "\trestje: huidige token_ttr= " << token_ttr;
#endif
      // partial result
      double threshold = ( 1 - token_ttr ) / (1 - settings.mtld_threshold);
#ifdef DEBUG_MTLD
      cerr << " dus verhoog de factor met " << (1 - token_ttr)
	   << "/" << ( 1 - settings.mtld_threshold) << endl;
#endif
      token_factor += threshold;
    }
  }
  if ( token_factor == 0.0 )
    token_factor = 1.0;
#ifdef DEBUG_MTLD
  cerr << "Factor = " << token_factor << " #words = " << v.size() << endl;
#endif
  return v.size() / token_factor;
}

double average_mtld( vector<string>& tokens ){
#ifdef DEBUG_MTLD
  cerr << "bereken MTLD van " << tokens << endl;
#endif
  double mtld1 = calculate_mtld( tokens );
#ifdef DEBUG_MTLD
  cerr << "VOORUIT = " << mtld1 << endl;
#endif
  reverse( tokens.begin(), tokens.end() );
  double mtld2 = calculate_mtld( tokens );
#ifdef DEBUG_MTLD
  cerr << "ACHTERUIT = " << mtld2 << endl;
#endif
  double result = (mtld1 + mtld2)/2.0;
#ifdef DEBUG_MTLD
  cerr << "average mtld = " << result << endl;
#endif
  return result;
}

void structStats::calculate_MTLDs() {
  const vector<const wordStats*> wordNodes = collectWords();
  vector<string> words;
  vector<string> lemmas;
  vector<string> conts;
  vector<string> names;
  vector<string> temp_conn;
  vector<string> reeks_wg_conn;
  vector<string> reeks_zin_conn;
  vector<string> contr_conn;
  vector<string> comp_conn;
  vector<string> cause_conn;
  vector<string> tijd_sits;
  vector<string> ruimte_sits;
  vector<string> cause_sits;
  vector<string> emotion_sits;
  for ( size_t i=0; i < wordNodes.size(); ++i ){
    if ( wordNodes[i]->wordProperty() == CGN::ISLET ){
      continue;
    }
    string word = wordNodes[i]->ltext();
    words.push_back( word );
    string lemma = wordNodes[i]->llemma();
    lemmas.push_back( lemma );
    if ( wordNodes[i]->isContent ){
      conts.push_back( wordNodes[i]->ltext() );
    }
    if ( wordNodes[i]->prop == CGN::ISNAME ){
      names.push_back( wordNodes[i]->ltext() );
    }
    switch( wordNodes[i]->getConnType() ){
    case Conn::TEMPOREEL:
      temp_conn.push_back( wordNodes[i]->ltext() );
      break;
    case Conn::OPSOMMEND_WG:
      reeks_wg_conn.push_back( wordNodes[i]->ltext() );
      break;
    case Conn::OPSOMMEND_ZIN:
      reeks_zin_conn.push_back( wordNodes[i]->ltext() );
      break;
    case Conn::CONTRASTIEF:
      contr_conn.push_back( wordNodes[i]->ltext() );
      break;
    case Conn::COMPARATIEF:
      comp_conn.push_back( wordNodes[i]->ltext() );
      break;
    case Conn::CAUSAAL:
      cause_conn.push_back( wordNodes[i]->ltext() );
      break;
    default:
      break;
    }
    switch( wordNodes[i]->getSitType() ){
    case Situation::TIME_SIT:
      tijd_sits.push_back(wordNodes[i]->Lemma());
      break;
    case Situation::CAUSAL_SIT:
      cause_sits.push_back(wordNodes[i]->Lemma());
      break;
    case Situation::SPACE_SIT:
      ruimte_sits.push_back(wordNodes[i]->Lemma());
      break;
    case Situation::EMO_SIT:
      emotion_sits.push_back(wordNodes[i]->Lemma());
      break;
    default:
      break;
    }
  }

  word_mtld = average_mtld( words );
  lemma_mtld = average_mtld( lemmas );
  content_mtld = average_mtld( conts );
  name_mtld = average_mtld( names );
  temp_conn_mtld = average_mtld( temp_conn );
  reeks_wg_conn_mtld = average_mtld( reeks_wg_conn );
  reeks_zin_conn_mtld = average_mtld( reeks_zin_conn );
  contr_conn_mtld = average_mtld( contr_conn );
  comp_conn_mtld = average_mtld( comp_conn );
  cause_conn_mtld = average_mtld( cause_conn );
  tijd_sit_mtld = average_mtld( tijd_sits );
  ruimte_sit_mtld = average_mtld( ruimte_sits );
  cause_sit_mtld = average_mtld( cause_sits );
  emotion_sit_mtld = average_mtld( emotion_sits );
}

void sentStats::setLSAvalues( double suc, double net, double ctx ){
  if ( suc > 0 )
    lsa_word_suc = suc;
  if ( net > 0 )
    lsa_word_net = net;
  if ( ctx > 0 )
    throw logic_error("context cannot be !=0 for sentstats");
}

double sentStats::getMeanAL() const {
  double result = NAN;
  size_t len = distances.size();
  if ( len > 0 ){
    result = 0;
    for( multimap<DD_type, int>::const_iterator pos = distances.begin();
	 pos != distances.end();
	 ++pos ){
      result += pos->second;
    }
    result = result/len;
  }
  return result;
}

double sentStats::getHighestAL() const {
  double result = 0;
  for( multimap<DD_type, int>::const_iterator pos = distances.begin();
       pos != distances.end();
       ++pos ){
    if ( pos->second > result )
      result = pos->second;
  }
  return result;
}

void fill_word_lemma_buffers( const sentStats* ss,
			      vector<string>& wv,
			      vector<string>& lv ){
  vector<basicStats*> bv = ss->sv;
  for ( size_t i=0; i < bv.size(); ++i ){
    wordStats *w = dynamic_cast<wordStats*>(bv[i]);
    if ( w->isOverlapCandidate() ){
      wv.push_back( w->l_word );
      lv.push_back( w->l_lemma );
    }
  }
}

void np_length( folia::Sentence *s, int& npcount, int& indefcount, int& size ) {
  vector<folia::Chunk *> cv = s->select<folia::Chunk>();
  size = 0 ;
  for( size_t i=0; i < cv.size(); ++i ){
    if ( cv[i]->cls() == "NP" ){
      ++npcount;
      size += cv[i]->size();
      folia::FoliaElement *det = cv[i]->index(0);
      if ( det ){
	vector<folia::PosAnnotation*> posV = det->select<folia::PosAnnotation>(frog_pos_set);
	if ( posV.size() != 1 )
	  throw folia::ValueError( "word doesn't have Frog POS tag info" );
	if ( posV[0]->feat("head") == "LID" ){
	  if ( det->text() == "een" )
	    ++indefcount;
	}
      }
    }
  }
}

bool sentStats::checkAls( size_t index ){
  static string compAlsList[] = { "net", "evenmin", "zo", "zomin" };
  static set<string> compAlsSet( compAlsList,
				 compAlsList + sizeof(compAlsList)/sizeof(string) );
  static string opsomAlsList[] = { "zowel" };
  static set<string> opsomAlsSet( opsomAlsList,
				  opsomAlsList + sizeof(opsomAlsList)/sizeof(string) );

  string als = sv[index]->ltext();
  if ( als == "als" ){
    if ( index == 0 ){
      // eerste woord, terugkijken kan dus niet
      sv[0]->setConnType( Conn::CAUSAAL );
    }
    else {
      for ( size_t i = index-1; i+1 != 0; --i ){
	string word = sv[i]->ltext();
	if ( compAlsSet.find( word ) != compAlsSet.end() ){
	  // kijk naar "evenmin ... als" constructies
	  sv[i]->setConnType( Conn::COMPARATIEF );
	  sv[index]->setConnType( Conn::COMPARATIEF );
	  //	cerr << "ALS comparatief:" << word << endl;
	  return true;
	}
	else if ( opsomAlsSet.find( word ) != opsomAlsSet.end() ){
	  // kijk naar "zowel ... als" constructies
	  sv[i]->setConnType( Conn::OPSOMMEND_WG );
	  sv[index]->setConnType( Conn::OPSOMMEND_WG );
	  //	cerr << "ALS opsommend:" << word << endl;
	  return true;
	}
      }
      if ( sv[index]->postag() == CGN::VG ){
	if ( sv[index-1]->postag() == CGN::ADJ ){
	  // "groter als"
	  //	cerr << "ALS comparatief: ADJ: " << sv[index-1]->text() << endl;
	  sv[index]->setConnType( Conn::COMPARATIEF );
	}
	else {
	  //	cerr << "ALS causaal: " << sv[index-1]->text() << endl;
	  sv[index]->setConnType( Conn::CAUSAAL );
	}
	return true;
      }
    }
    if ( index < sv.size() &&
	 sv[index+1]->postag() == CGN::TW ){
      // "als eerste" "als dertigste"
      sv[index]->setConnType( Conn::COMPARATIEF );
      return true;
    }
  }
  return false;
}

Conn::Type sentStats::checkMultiConnectives( const string& mword ){
  Conn::Type conn = Conn::NOCONN;
  if ( settings.multi_temporals.find( mword ) != settings.multi_temporals.end() ){
    conn = Conn::TEMPOREEL;
  }
  else if ( settings.multi_opsommers_wg.find( mword ) != settings.multi_opsommers_wg.end() ){
    conn = Conn::OPSOMMEND_WG;
  }
  else if ( settings.multi_opsommers_zin.find( mword ) != settings.multi_opsommers_zin.end() ){
    conn = Conn::OPSOMMEND_ZIN;
  }
  else if ( settings.multi_contrast.find( mword ) != settings.multi_contrast.end() ){
    conn = Conn::CONTRASTIEF;
  }
  else if ( settings.multi_compars.find( mword ) != settings.multi_compars.end() ){
    conn = Conn::COMPARATIEF;
  }
  else if ( settings.multi_causals.find( mword ) != settings.multi_causals.end() ){
    conn = Conn::CAUSAAL;
  }
  //  cerr << "multi-conn " << mword << " = " << conn << endl;
  return conn;
}

Situation::Type sentStats::checkMultiSituations( const string& mword ){
  //  cerr << "check multi-sit '" << mword << "'" << endl;
  Situation::Type sit = Situation::NO_SIT;
  if ( settings.multi_time_sits.find( mword ) != settings.multi_time_sits.end() ){
    sit = Situation::TIME_SIT;
  }
  else if ( settings.multi_space_sits.find( mword ) != settings.multi_space_sits.end() ){
    sit = Situation::SPACE_SIT;
  }
  else if ( settings.multi_causal_sits.find( mword ) != settings.multi_causal_sits.end() ){
    sit = Situation::CAUSAL_SIT;
  }
  else if ( settings.multi_emotion_sits.find( mword ) != settings.multi_emotion_sits.end() ){
    sit = Situation::EMO_SIT;
  }
  //  cerr << "multi-sit " << mword << " = " << sit << endl;
  return sit;
}

void sentStats::incrementConnCnt( Conn::Type t ){
  switch ( t ){
  case Conn::TEMPOREEL:
    tempConnCnt++;
    break;
  case Conn::OPSOMMEND_WG:
    opsomWgConnCnt++;
    break;
  case Conn::OPSOMMEND_ZIN:
    opsomZinConnCnt++;
    break;
  case Conn::CONTRASTIEF:
    contrastConnCnt++;
    break;
  case Conn::COMPARATIEF:
    compConnCnt++;
    break;
  case Conn::CAUSAAL:
    break;
  default:
    break;
  }
}

const string neg_longA[] = { "afgezien van",
           "zomin als",
           "met uitzondering van"};
static set<string> negatives_long = set<string>( neg_longA,
             neg_longA + sizeof(neg_longA)/sizeof(string) );

void sentStats::resolveConnectives(){
  if ( sv.size() > 1 ){
    for ( size_t i=0; i < sv.size()-2; ++i ){
      string word = sv[i]->ltext();
      string multiword2 = word + " " + sv[i+1]->ltext();
      if ( !checkAls( i ) ){
	// "als" is speciaal als het matcht met eerdere woorden.
	// (evenmin ... als) (zowel ... als ) etc.
	// In dat geval niet meer zoeken naar "als ..."
	//      cerr << "zoek op " << multiword2 << endl;
	Conn::Type conn = checkMultiConnectives( multiword2 );
	if ( conn != Conn::NOCONN ){
	  sv[i]->setMultiConn();
	  sv[i+1]->setMultiConn();
	  sv[i]->setConnType( conn );
	  sv[i+1]->setConnType( Conn::NOCONN );
	}
      }
      if ( negatives_long.find( multiword2 ) != negatives_long.end() ){
	propNegCnt++;
      }
      string multiword3 = multiword2 + " " + sv[i+2]->ltext();
      //      cerr << "zoek op " << multiword3 << endl;
      Conn::Type conn = checkMultiConnectives( multiword3 );
      if ( conn != Conn::NOCONN ){
	sv[i]->setMultiConn();
	sv[i+1]->setMultiConn();
	sv[i+2]->setMultiConn();
	sv[i]->setConnType( conn );
	sv[i+1]->setConnType( Conn::NOCONN );
	sv[i+2]->setConnType( Conn::NOCONN );
      }
      if ( negatives_long.find( multiword3 ) != negatives_long.end() )
	propNegCnt++;
    }
    // don't forget the last 2 words
    string multiword2 = sv[sv.size()-2]->ltext() + " "
      + sv[sv.size()-1]->ltext();
    //    cerr << "zoek op " << multiword2 << endl;
    Conn::Type conn = checkMultiConnectives( multiword2 );
    if ( conn != Conn::NOCONN ){
      sv[sv.size()-2]->setMultiConn();
      sv[sv.size()-1]->setMultiConn();
      sv[sv.size()-2]->setConnType( conn );
      sv[sv.size()-1]->setConnType( Conn::NOCONN );
    }
    if ( negatives_long.find( multiword2 ) != negatives_long.end() ){
      propNegCnt++;
    }
  }
  for ( size_t i=0; i < sv.size(); ++i ){
    switch( sv[i]->getConnType() ){
    case Conn::TEMPOREEL:
      unique_temp_conn[sv[i]->ltext()]++;
      tempConnCnt++;
      break;
    case Conn::OPSOMMEND_WG:
      unique_reeks_wg_conn[sv[i]->ltext()]++;
      opsomWgConnCnt++;
      break;
    case Conn::OPSOMMEND_ZIN:
      unique_reeks_zin_conn[sv[i]->ltext()]++;
      opsomZinConnCnt++;
      break;
    case Conn::CONTRASTIEF:
      unique_contr_conn[sv[i]->ltext()]++;
      contrastConnCnt++;
      break;
    case Conn::COMPARATIEF:
      unique_comp_conn[sv[i]->ltext()]++;
      compConnCnt++;
      break;
    case Conn::CAUSAAL:
      unique_cause_conn[sv[i]->ltext()]++;
      causeConnCnt++;
      break;
    default:
      break;
    }
  }
}

void sentStats::resolveSituations(){
  if ( sv.size() > 1 ){
    for ( size_t i=0; (i+3) < sv.size(); ++i ){
      string word = sv[i]->Lemma();
      string multiword2 = word + " " + sv[i+1]->Lemma();
      string multiword3 = multiword2 + " " + sv[i+2]->Lemma();
      string multiword4 = multiword3 + " " + sv[i+3]->Lemma();
      //      cerr << "zoek 4 op '" << multiword4 << "'" << endl;
      Situation::Type sit = checkMultiSituations( multiword4 );
      if ( sit != Situation::NO_SIT ){
	//	cerr << "found " << sit << "-situation: " << multiword4 << endl;
	sv[i]->setSitType( Situation::NO_SIT );
	sv[i+1]->setSitType( Situation::NO_SIT );
	sv[i+2]->setSitType( Situation::NO_SIT );
	sv[i+3]->setSitType( sit );
	i += 3;
      }
      else {
	//cerr << "zoek 3 op '" << multiword3 << "'" << endl;
	sit = checkMultiSituations( multiword3 );
	if ( sit != Situation::NO_SIT ){
	  // cerr << "found " << sit << "-situation: " << multiword3 << endl;
	  sv[i]->setSitType( Situation::NO_SIT );
	  sv[i+1]->setSitType( Situation::NO_SIT );
	  sv[i+2]->setSitType( sit );
	  i += 2;
	}
	else {
	  //cerr << "zoek 2 op '" << multiword2 << "'" << endl;
	  sit = checkMultiSituations( multiword2 );
	  if ( sit != Situation::NO_SIT ){
	    //	    cerr << "found " << sit << "-situation: " << multiword2 << endl;
	    sv[i]->setSitType( Situation::NO_SIT);
	    sv[i+1]->setSitType( sit );
	    i += 1;
	  }
	}
      }
    }
    // don't forget the last 2 and 3 words
    Situation::Type sit = Situation::NO_SIT;
    if ( sv.size() > 2 ){
      string multiword3 = sv[sv.size()-3]->Lemma() + " "
	+ sv[sv.size()-2]->Lemma() + " " + sv[sv.size()-1]->Lemma();
      //cerr << "zoek final 3 op '" << multiword3 << "'" << endl;
      sit = checkMultiSituations( multiword3 );
      if ( sit != Situation::NO_SIT ){
	//	cerr << "found " << sit << "-situation: " << multiword3 << endl;
	sv[sv.size()-3]->setSitType( Situation::NO_SIT );
	sv[sv.size()-2]->setSitType( Situation::NO_SIT );
	sv[sv.size()-1]->setSitType( sit );
      }
      else {
	string multiword2 = sv[sv.size()-3]->Lemma() + " "
	  + sv[sv.size()-2]->Lemma();
	//cerr << "zoek first final 2 op '" << multiword2 << "'" << endl;
	sit = checkMultiSituations( multiword2 );
	if ( sit != Situation::NO_SIT ){
	  //	  cerr << "found " << sit << "-situation: " << multiword2 << endl;
	  sv[sv.size()-3]->setSitType( Situation::NO_SIT);
	  sv[sv.size()-2]->setSitType( sit );
	}
	else {
	  string multiword2 = sv[sv.size()-2]->Lemma() + " "
	    + sv[sv.size()-1]->Lemma();
	  //cerr << "zoek second final 2 op '" << multiword2 << "'" << endl;
	  sit = checkMultiSituations( multiword2 );
	  if ( sit != Situation::NO_SIT ){
	    //	    cerr << "found " << sit << "-situation: " << multiword2 << endl;
	    sv[sv.size()-2]->setSitType( Situation::NO_SIT);
	    sv[sv.size()-1]->setSitType( sit );
	  }
	}
      }
    }
    else {
      string multiword2 = sv[sv.size()-2]->Lemma() + " "
	+ sv[sv.size()-1]->Lemma();
      // cerr << "zoek second final 2 op '" << multiword2 << "'" << endl;
      sit = checkMultiSituations( multiword2 );
      if ( sit != Situation::NO_SIT ){
	//	cerr << "found " << sit << "-situation: " << multiword2 << endl;
	sv[sv.size()-2]->setSitType( Situation::NO_SIT);
	sv[sv.size()-1]->setSitType( sit );
      }
    }
  }
  for ( size_t i=0; i < sv.size(); ++i ){
    switch( sv[i]->getSitType() ){
    case Situation::TIME_SIT:
      unique_tijd_sits[sv[i]->Lemma()]++;
      timeSitCnt++;
      break;
    case Situation::CAUSAL_SIT:
      unique_cause_sits[sv[i]->Lemma()]++;
      causeSitCnt++;
      break;
    case Situation::SPACE_SIT:
      unique_ruimte_sits[sv[i]->Lemma()]++;
      spaceSitCnt++;
      break;
    case Situation::EMO_SIT:
      unique_emotion_sits[sv[i]->Lemma()]++;
      emoSitCnt++;
      break;
    default:
      break;
    }
  }
}

void sentStats::resolveLSA( const map<string,double>& LSAword_dists ){
  if ( sv.size() < 1 )
    return;

  int lets = 0;
  double suc = 0;
  double net = 0;
  size_t node_count = 0;
  for ( size_t i=0; i < sv.size(); ++i ){
    double context = 0.0;
    for ( size_t j=0; j < sv.size(); ++j ){
      if ( j == i )
	continue;
      string word1 = sv[i]->ltext();
      string word2 = sv[j]->ltext();
      string call = word1 + "\t" + word2;
      map<string,double>::const_iterator it = LSAword_dists.find(call);
      if ( it != LSAword_dists.end() ){
	context += it->second;
      }
    }
    sv[i]->setLSAcontext(context/(sv.size()-1));
    for ( size_t j=i+1; j < sv.size(); ++j ){
      if ( sv[i]->wordProperty() == CGN::ISLET ){
	continue;
      }
      if ( sv[j]->wordProperty() == CGN::ISLET ){
	if ( i == 0 )
	  ++lets;
	continue;
      }
      ++node_count;
      string word1 = sv[i]->ltext();
      string word2 = sv[j]->ltext();
      string call = word1 + "\t" + word2;
      double result = 0;
      map<string,double>::const_iterator it = LSAword_dists.find(call);
      if ( it != LSAword_dists.end() ){
	result = it->second;
	if ( j == i+1 ){
	  sv[i]->setLSAsuc(result);
	  suc += result;
	}
	net += result;
      }
#ifdef DEBUG_LSA
      cerr << "LSA-sent lookup '" << call << "' ==> " << result << endl;
#endif
    }
  }
#ifdef DEBUG_LSA
  cerr << "size = " << sv.size() << ", LETS = " << lets << endl;
  cerr << "LSA-sent-SUC sum = " << suc << endl;
  cerr << "LSA-sent=NET sum = " << net << ", node count = " << node_count << endl;
#endif
  suc = suc/(sv.size()-lets);
  net = net/node_count;
#ifdef DEBUG_LSA
  cerr << "LSA-sent-SUC result = " << suc << endl;
  cerr << "LSA-sent-NET result = " << net << endl;
#endif
  setLSAvalues( suc, net, 0 );
}

void sentStats::resolveMultiWordIntensify(){
  size_t max_length_intensify = 5;
  for ( size_t i = 0; i < sv.size() - 1; ++i ){
    string startword = sv[i]->text();
    string multiword = startword;

    for ( size_t j = 1; i + j < sv.size() && j < max_length_intensify; ++j ){
      // Attach the next word to the expression
      multiword += " " + sv[i+j]->text();

      // Look for the expression in the list of intensifiers
      map<string,Intensify::Type>::const_iterator sit;
      sit = settings.intensify.find( multiword );
      // If found, update the counts, if not, continue
      if ( sit != settings.intensify.end() ){
        intensCombiCnt += j + 1;
        intensCnt += j + 1;
        // Break and skip to the first word after this expression
        i += j;
        break;
      }
    }
  }
}

void sentStats::resolveMultiWordAfks(){
  if ( sv.size() > 1 ){
    for ( size_t i=0; i < sv.size()-2; ++i ){
      string word = sv[i]->text();
      string multiword2 = word + " " + sv[i+1]->text();
      string multiword3 = multiword2 + " " + sv[i+2]->text();
      Afk::Type at = Afk::NO_A;
      map<string,Afk::Type>::const_iterator sit
	= settings.afkos.find( multiword3 );
      if ( sit == settings.afkos.end() ){
	sit = settings.afkos.find( multiword2 );
      }
      else {
	cerr << "FOUND a 3-word AFK: '" << multiword3 << "'" << endl;
      }
      if ( sit != settings.afkos.end() ){
	cerr << "FOUND a 2-word AFK: '" << multiword2 << "'" << endl;
	at = sit->second;
      }
      if ( at != Afk::NO_A ){
	++afks[at];
      }
    }
    // don't forget the last 2 words
    string multiword2 = sv[sv.size()-2]->text() + " " + sv[sv.size()-1]->text();
    map<string,Afk::Type>::const_iterator sit
      = settings.afkos.find( multiword2 );
    if ( sit != settings.afkos.end() ){
      cerr << "FOUND a 2-word AFK: '" << multiword2 << "'" << endl;
      Afk::Type at = sit->second;
      ++afks[at];
    }
  }
}

void sentStats::resolvePrepExpr(){
  if ( sv.size() > 2 ){
    for ( size_t i=0; i < sv.size()-1; ++i ){
      string word = sv[i]->ltext();
      string mw2 = word + " " + sv[i+1]->ltext();
      if ( settings.vzexpr2.find( mw2 ) != settings.vzexpr2.end() ){
	++prepExprCnt;
	i += 1;
	continue;
      }
      if ( i < sv.size() - 2 ){
	string mw3 = mw2 + " " + sv[i+2]->ltext();
	if ( settings.vzexpr3.find( mw3 ) != settings.vzexpr3.end() ){
	  ++prepExprCnt;
	  i += 2;
	  continue;
	}
	if ( i < sv.size() - 3 ){
	  string mw4 = mw3 + " " + sv[i+3]->ltext();
	  if ( settings.vzexpr4.find( mw4 ) != settings.vzexpr4.end() ){
	    ++prepExprCnt;
	    i += 3;
	    continue;
	  }
	}
      }
    }
  }
}

// Finds nodes of adverbials and reports counts
void sentStats::resolveAdverbials(xmlDoc *alpDoc) {
  list<xmlNode*> nodes = getAdverbialNodes(alpDoc);
  vcModCnt = nodes.size();

  // Check for adverbials consisting of a single node that has the 'GENERAL' type.
  for (auto& node : nodes) {
    string word = getAttribute(node, "word");
    if (word != "")
    {
      word = TiCC::lowercase(word);
      if (checkAdverbType(word, CGN::BW) == Adverb::GENERAL)
      {
        vcModSingleCnt++;
      }
    }
  }
}

// Finds nodes of relative clauses and reports counts
void sentStats::resolveRelativeClauses(xmlDoc *alpDoc) {
  string hasFiniteVerb = "//node[@cat='ssub']";
  // Betrekkelijke/bijvoeglijke bijzinnen (zonder/met nevenschikking)
  list<xmlNode*> relNodes = getNodesByRelCat(alpDoc, "mod", "rel", hasFiniteVerb);
  relNodes.merge(getNodesByRelCat(alpDoc, "mod", "whrel", hasFiniteVerb));
  string relConjPath = ".//node[@rel='mod' and @cat='conj']//node[@rel='cnj' and (@cat='rel' or @cat='whrel')]";
  relNodes.merge(TiCC::FindNodes(alpDoc, relConjPath + hasFiniteVerb));
  // Bijwoordelijke bijzinnen (zonder/met nevenschikking)
  list<xmlNode*> cpNodes = getNodesByRelCat(alpDoc, "mod", "cp", hasFiniteVerb);
  string cpConjPath = ".//node[@rel='mod' and @cat='conj']//node[@rel='cnj' and @cat='cp']";
  cpNodes.merge(TiCC::FindNodes(alpDoc, cpConjPath + hasFiniteVerb));
  // Finiete complementszinnen
  // Check whether the previous node is not the top node to prevent clashes with loose clauses below
  string complPath = ".//node[@cat!='top' and @rel!='mod']/node[@rel!='mod' and (@cat='whsub' or @cat='whrel' or @cat='cp')]";
  list<xmlNode*> complNodes = TiCC::FindNodes(alpDoc, complPath + hasFiniteVerb);
  // Infinietcomplementen
  list<xmlNode*> tiNodes = getNodesByCat(alpDoc, "ti");

  betrCnt = relNodes.size();
  bijwCnt = cpNodes.size();
  complCnt = complNodes.size();
  infinComplCnt = tiNodes.size();

  // Checks for embedded finite clauses
  list<xmlNode*> allRelNodes (relNodes);
  allRelNodes.merge(cpNodes);
  allRelNodes.merge(complNodes);
  list<string> ids;
  for (auto& node : allRelNodes) {
    ids.merge(getNodeIds(getNodesByRelCat(node, "mod", "rel", hasFiniteVerb)));
    ids.merge(getNodeIds(getNodesByRelCat(node, "mod", "whrel", hasFiniteVerb)));
    ids.merge(getNodeIds(TiCC::FindNodes(node, relConjPath + hasFiniteVerb)));
    ids.merge(getNodeIds(getNodesByRelCat(node, "mod", "cp", hasFiniteVerb)));
    ids.merge(getNodeIds(TiCC::FindNodes(node, cpConjPath + hasFiniteVerb)));
    ids.merge(getNodeIds(TiCC::FindNodes(node, complPath + hasFiniteVerb)));
  }
  set<string> mvFinEmbedIds(ids.begin(), ids.end());
  mvFinInbedCnt = mvFinEmbedIds.size();

  // Checks for all embedded clauses
  allRelNodes.merge(tiNodes);
  ids.clear();
  for (auto& node : allRelNodes) {
    ids.merge(getNodeIds(getNodesByRelCat(node, "mod", "rel", hasFiniteVerb)));
    ids.merge(getNodeIds(getNodesByRelCat(node, "mod", "whrel", hasFiniteVerb)));
    ids.merge(getNodeIds(TiCC::FindNodes(node, relConjPath + hasFiniteVerb)));
    ids.merge(getNodeIds(getNodesByRelCat(node, "mod", "cp", hasFiniteVerb)));
    ids.merge(getNodeIds(TiCC::FindNodes(node, cpConjPath + hasFiniteVerb)));
    ids.merge(getNodeIds(TiCC::FindNodes(node, complPath + hasFiniteVerb)));
    ids.merge(getNodeIds(getNodesByCat(node, "ti")));
  }
  set<string> mvInbedIds(ids.begin(), ids.end());
  mvInbedCnt = mvInbedIds.size();

  // Count 'loose' (directly under top node) relative clauses
  string losBetr = "//node[@cat='top']/node[@cat='rel' or @cat='whrel']" + hasFiniteVerb;
  losBetrCnt = TiCC::FindNodes(alpDoc, losBetr).size();
  string losBijw = "//node[@cat='top']/node[@cat='cp']" + hasFiniteVerb;
  losBijwCnt = TiCC::FindNodes(alpDoc, losBijw).size();
}

// Finds nodes of finite verbs and reports counts
void sentStats::resolveFiniteVerbs(xmlDoc *alpDoc) {
  smainCnt = getNodesByCat(alpDoc, "smain").size();
  ssubCnt = getNodesByCat(alpDoc, "ssub").size();
  sv1Cnt = getNodesByCat(alpDoc, "sv1").size();

  clauseCnt = smainCnt + ssubCnt + sv1Cnt;
  correctedClauseCnt = clauseCnt > 0 ? clauseCnt : 1; // Correct clause count to 1 if there are no verbs in the sentence
}

// Finds nodes of coordinating conjunctions and reports counts
void sentStats::resolveConjunctions(xmlDoc *alpDoc) {
  smainCnjCnt = getNodesByRelCat(alpDoc, "cnj", "smain").size();
  // For cnj-ssub, also allow that the cnj node dominates the ssub node
  ssubCnjCnt = TiCC::FindNodes(alpDoc, ".//node[@rel='cnj'][descendant-or-self::node[@cat='ssub']]").size();
  sv1CnjCnt = getNodesByRelCat(alpDoc, "cnj", "sv1").size();
}

//#define DEBUG_WOPR
void orderWopr( const string& txt, vector<double>& wordProbsV,
		double& sentProb, double& entropy, double& perplexity ){
  string host = config.lookUp( "host", "wopr" );
  string port = config.lookUp( "port", "wopr" );
  Sockets::ClientSocket client;
  if ( !client.connect( host, port ) ){
    cerr << "failed to open Wopr connection: "<< host << ":" << port << endl;
    cerr << "Reason: " << client.getMessage() << endl;
    exit( EXIT_FAILURE );
  }
  cerr << "calling Wopr" << endl;
  client.write( txt + "\n\n" );
  string result;
  string s;
  while ( client.read(s) ){
    result += s + "\n";
  }
#ifdef DEBUG_WOPR
  cerr << "received data [" << result << "]" << endl;
#endif
  if ( !result.empty() && result.size() > 10 ){
#ifdef DEBUG_WOPR
    cerr << "start FoLiA parsing" << endl;
#endif
    folia::Document *doc = new folia::Document();
    try {
      doc->readFromString( result );
#ifdef DEBUG_WOPR
      cerr << "finished parsing" << endl;
#endif
      vector<folia::Word*> wv = doc->words();
      if ( wv.size() !=  wordProbsV.size() ){
	cerr << "unforseen mismatch between de number of words returned by WOPR"
	    << endl << " and the number of words in the input sentence. "
	    << endl;
	return;
      }
      for ( size_t i=0; i < wv.size(); ++i ){
	vector<folia::Metric*> mv = wv[i]->select<folia::Metric>();
	if ( mv.size() > 0 ){
	  for ( size_t j=0; j < mv.size(); ++j ){
	    if ( mv[j]->cls() == "lprob10" ){
	      wordProbsV[i] = TiCC::stringTo<double>( mv[j]->feat("value") );
	    }
	  }
	}
      }
      vector<folia::Sentence*> sv = doc->sentences();
      if ( sv.size() != 1 ){
	throw logic_error( "The document returned by WOPR contains > 1 Sentence" );
	return;
      }
      vector<folia::Metric*> mv = sv[0]->select<folia::Metric>();
      if ( mv.size() > 0 ){
	for ( size_t j=0; j < mv.size(); ++j ){
	  if ( mv[j]->cls() == "avg_prob10" ){
	    string val = mv[j]->feat("value");
	    if ( val != "nan" ){
	      sentProb = TiCC::stringTo<double>( val );
	    }
	  }
	  else if ( mv[j]->cls() == "entropy" ){
	    string val = mv[j]->feat("value");
	    if ( val != "nan" ){
	      entropy = TiCC::stringTo<double>( val );
	    }
	  }
	  else if ( mv[j]->cls() == "perplexity" ){
	    string val = mv[j]->feat("value");
	    if ( val != "nan" ){
	      perplexity = TiCC::stringTo<double>( val );
	    }
	  }
	}
      }
    }
    catch ( std::exception& e ){
      cerr << "FoLiaParsing failed:" << endl
	  << e.what() << endl;
    }
  }
  else {
    cerr << "No usable FoLia date retrieved from Wopr. Got '"
	<< result << "'" << endl;
  }
  cerr << "Done with Wopr" << endl;
}

xmlDoc *AlpinoServerParse( folia::Sentence *);

sentStats::sentStats( int index, folia::Sentence *s, const sentStats* pred,
		      const map<string,double>& LSAword_dists ):
  structStats( index, s, "sent" ){
  text = folia::UnicodeToUTF8( s->toktext() );
  cerr << "analyse tokenized sentence=" << text << endl;
  vector<folia::Word*> w = s->words();
  vector<double> woprProbsV(w.size(),NAN);
  double sentProb = NAN;
  double sentEntropy = NAN;
  double sentPerplexity = NAN;
  xmlDoc *alpDoc = 0;
  set<size_t> puncts;
  parseFailCnt = -1; // not parsed (yet)
#pragma omp parallel sections
  {
#pragma omp section
    {
      if ( settings.doAlpino || settings.doAlpinoServer ){
	if ( settings.doAlpinoServer ){
	  cerr << "calling Alpino Server" << endl;
	  alpDoc = AlpinoServerParse( s );
	  if ( !alpDoc ){
	    cerr << "alpino parser failed!" << endl;
	  }
	  cerr << "done with Alpino Server" << endl;
	}
	else if ( settings.doAlpino ){
	  cerr << "calling Alpino parser" << endl;
	  alpDoc = AlpinoParse( s, workdir_name );
	  if ( !alpDoc ){
	    cerr << "alpino parser failed!" << endl;
	  }
	  cerr << "done with Alpino parser" << endl;
	}
	if ( alpDoc ){
	  parseFailCnt = 0; // OK
	  for( size_t i=0; i < w.size(); ++i ){
	    vector<folia::PosAnnotation*> posV = w[i]->select<folia::PosAnnotation>(frog_pos_set);
	    if ( posV.size() != 1 )
	      throw folia::ValueError( "word doesn't have Frog POS tag info" );
	    folia::PosAnnotation *pa = posV[0];
	    string posHead = pa->feat("head");
	    if ( posHead == "LET" ){
	      puncts.insert( i );
	    }
	  }
	  dLevel = get_d_level( s, alpDoc );
	  if ( dLevel > 4 )
	    dLevel_gt4 = 1;
	  mod_stats( alpDoc, adjNpModCnt, npModCnt );
    resolveAdverbials(alpDoc);
    resolveRelativeClauses(alpDoc);
    resolveFiniteVerbs(alpDoc);
    resolveConjunctions(alpDoc);
	}
	else {
	  parseFailCnt = 1; // failed
	}
      }
    } // omp section

#pragma omp section
    {
      if ( settings.doWopr ){
	orderWopr( text, woprProbsV, sentProb, sentEntropy, sentPerplexity );
      }
    } // omp section
  } // omp sections

  // if ( parseFailCnt == 1 ){
  //   // glorious fail
  //   return;
  // }
  sentCnt = 1; // so only count the sentence when not failed
  if ( sentProb != -99 ){
    avg_prob10 = sentProb;
  }
  entropy = sentEntropy;
  perplexity = sentPerplexity;
  //  cerr << "PUNCTS " << puncts << endl;
  bool question = false;
  vector<string> wordbuffer;
  vector<string> lemmabuffer;
  if ( pred ){
    fill_word_lemma_buffers( pred, wordbuffer, lemmabuffer );
#ifdef DEBUG_OL
    cerr << "call sentenceOverlap, wordbuffer " << wordbuffer << endl;
    cerr << "call sentenceOverlap, lemmabuffer " << lemmabuffer << endl;
#endif
  }
  for ( size_t i=0; i < w.size(); ++i ){
    xmlNode *alpWord = 0;
    if ( alpDoc ){
      alpWord = getAlpNodeWord( alpDoc, w[i] );
    }
    wordStats *ws = new wordStats( i, w[i], alpWord, puncts, parseFailCnt==1 );
    if ( parseFailCnt ){
      sv.push_back( ws );
      continue;
    }
    if ( woprProbsV[i] != -99 )
      ws->logprob10 = woprProbsV[i];
    if ( pred ){
      ws->getSentenceOverlap( wordbuffer, lemmabuffer );
    }

    if ( ws->lemma[ws->lemma.length()-1] == '?' ){
      question = true;
    }
    if ( ws->prop == CGN::ISLET ){
      letCnt++;
      sv.push_back( ws );
      continue;
    }
    else {
      NER::Type ner = NER::lookupNer( w[i], s );
      ws->nerProp = ner;
      switch( ner ){
      case NER::LOC_B:
      case NER::EVE_B:
      case NER::MISC_B:
      case NER::ORG_B:
      case NER::PER_B:
      case NER::PRO_B:
	ners[ner]++;
	nerCnt++;
	break;
      default:
	;
      }
      ws->setPersRef(); // need NER Info for this
      wordCnt++;
      heads[ws->tag]++;
      if ( ws->afkType != Afk::NO_A ){
	++afks[ws->afkType];
      }
      wordOverlapCnt += ws->wordOverlapCnt;
      lemmaOverlapCnt += ws->lemmaOverlapCnt;
      charCnt += ws->charCnt;
      charCntExNames += ws->charCntExNames;
      morphCnt += ws->morphCnt;
      morphCntExNames += ws->morphCntExNames;
      unique_words[ws->l_word] += 1;
      unique_lemmas[ws->lemma] += 1;
      aggregate( distances, ws->distances );
      if ( ws->isContent ) {
        word_freq += ws->word_freq_log;
        lemma_freq += ws->lemma_freq_log;
        if ( ws->prop != CGN::ISNAME ){
          word_freq_n += ws->word_freq_log;
          lemma_freq_n += ws->lemma_freq_log;
        }
      }
      switch ( ws->prop ){
      case CGN::ISNAME:
	nameCnt++;
	unique_names[ws->l_word] +=1;
	break;
      case CGN::ISVD:
	switch ( ws->position ){
	case CGN::NOMIN:
	  vdNwCnt++;
	  break;
	case CGN::PRENOM:
	  vdBvCnt++;
	  break;
	case CGN::VRIJ:
	  vdVrijCnt++;
	  break;
	default:
	  break;
	}
	break;
      case CGN::ISINF:
	switch ( ws->position ){
	case CGN::NOMIN:
	  infNwCnt++;
	  break;
	case CGN::PRENOM:
	  infBvCnt++;
	  break;
	case CGN::VRIJ:
	  infVrijCnt++;
	  break;
	default:
	  break;
	}
	break;
      case CGN::ISOD:
	switch ( ws->position ){
	case CGN::NOMIN:
	  odNwCnt++;
	  break;
	case CGN::PRENOM:
	  odBvCnt++;
	  break;
	case CGN::VRIJ:
	  odVrijCnt++;
	  break;
	default:
	  break;
	}
	break;
      case CGN::ISPVVERL:
	pastCnt++;
	break;
      case CGN::ISPVTGW:
	presentCnt++;
	break;
      case CGN::ISSUBJ:
	subjonctCnt++;
	break;
      case CGN::ISPPRON1:
	pron1Cnt++;
	break;
      case CGN::ISPPRON2:
	pron2Cnt++;
	break;
      case CGN::ISPPRON3:
	pron3Cnt++;
	break;
      default:
	;// ignore JUSTAWORD and ISAANW
      }
      if ( ws->wwform == PASSIVE_VERB )
	passiveCnt++;
      if ( ws->wwform == MODAL_VERB )
	modalCnt++;
      if ( ws->wwform == TIME_VERB )
	timeVCnt++;
      if ( ws->wwform == COPULA )
	koppelCnt++;
      if ( ws->isPersRef )
	persRefCnt++;
      if ( ws->isPronRef )
	pronRefCnt++;
      if ( ws->archaic )
	archaicsCnt++;
      if ( ws->isContent ){
	contentCnt++;
	unique_contents[ws->l_word] +=1;
      }
      if ( ws->isNominal )
	nominalCnt++;
      switch ( ws->tag ){
      case CGN::N:
	nounCnt++;
	break;
      case CGN::ADJ:
	adjCnt++;
	break;
      case CGN::WW:
	verbCnt++;
	break;
      case CGN::VG:
	vgCnt++;
	break;
      case CGN::TSW:
	tswCnt++;
	break;
      case CGN::LET:
	letCnt++;
	break;
      case CGN::SPEC:
	specCnt++;
	break;
      case CGN::BW:
	bwCnt++;
	break;
      case CGN::VNW:
	vnwCnt++;
	break;
      case CGN::LID:
	lidCnt++;
	break;
      case CGN::TW:
	twCnt++;
	break;
      case CGN::VZ:
	vzCnt++;
	break;
      default:
	break;
      }
      if ( ws->isImperative )
	impCnt++;
      if ( ws->isPropNeg )
	propNegCnt++;
      if ( ws->isMorphNeg )
	morphNegCnt++;
      if ( ws->f50 )
	f50Cnt++;
      if ( ws->f65 )
	f65Cnt++;
      if ( ws->f77 )
	f77Cnt++;
      if ( ws->f80 )
	f80Cnt++;
      switch ( ws->top_freq ){
	// NO BREAKS (being in top1000 means being in top2000 as well)
      case top1000:
	++top1000Cnt;
  if ( ws->isContent ) ++top1000ContentCnt;
      case top2000:
	++top2000Cnt;
  if ( ws->isContent ) ++top2000ContentCnt;
      case top3000:
	++top3000Cnt;
  if ( ws->isContent ) ++top3000ContentCnt;
      case top5000:
	++top5000Cnt;
  if ( ws->isContent ) ++top5000ContentCnt;
      case top10000:
	++top10000Cnt;
  if ( ws->isContent ) ++top10000ContentCnt;
      case top20000:
	++top20000Cnt;
  if ( ws->isContent ) ++top20000ContentCnt;
      default:
	break;
      }
      switch ( ws->sem_type ){
      case SEM::UNDEFINED_NOUN:
	++undefinedNounCnt;
	break;
      case SEM::UNDEFINED_ADJ:
	++undefinedAdjCnt;
	break;
      case SEM::UNFOUND_NOUN:
	++uncoveredNounCnt;
	break;
      case SEM::UNFOUND_ADJ:
	++uncoveredAdjCnt;
	break;
      case SEM::UNFOUND_VERB:
	++uncoveredVerbCnt;
	break;
      case SEM::CONCRETE_HUMAN_NOUN:
	humanCnt++;
	strictNounCnt++;
	broadNounCnt++;
	break;
      case SEM::CONCRETE_NONHUMAN_NOUN:
	nonHumanCnt++;
	strictNounCnt++;
	broadNounCnt++;
	break;
      case SEM::CONCRETE_ARTEFACT_NOUN:
	artefactCnt++;
	strictNounCnt++;
	broadNounCnt++;
	break;
      case SEM::CONCRETE_SUBSTANCE_NOUN:
	substanceConcCnt++;
	strictNounCnt++;
	broadNounCnt++;
	break;
      case SEM::CONCRETE_FOOD_CARE_NOUN:
  foodcareCnt++;
  strictNounCnt++;
  broadNounCnt++;
  break;
      case SEM::CONCRETE_OTHER_NOUN:
	concrotherCnt++;
	strictNounCnt++;
	broadNounCnt++;
	break;
      case SEM::BROAD_CONCRETE_PLACE_NOUN:
	++placeCnt;
	broadNounCnt++;
	break;
      case SEM::BROAD_CONCRETE_TIME_NOUN:
	++timeCnt;
	broadNounCnt++;
	break;
      case SEM::BROAD_CONCRETE_MEASURE_NOUN:
	++measureCnt;
	broadNounCnt++;
	break;
      case SEM::CONCRETE_DYNAMIC_NOUN:
  ++dynamicConcCnt;
  strictNounCnt++;
  broadNounCnt++;
  break;
      case SEM::ABSTRACT_SUBSTANCE_NOUN:
  ++substanceAbstrCnt;
  break;
      case SEM::ABSTRACT_DYNAMIC_NOUN:
	++dynamicAbstrCnt;
	break;
      case SEM::ABSTRACT_NONDYNAMIC_NOUN:
	++nonDynamicCnt;
	break;
      case SEM::INSTITUT_NOUN:
	institutCnt++;
	break;
      case SEM::HUMAN_ADJ:
	humanAdjCnt++;
	broadAdjCnt++;
	strictAdjCnt++;
	break;
      case SEM::EMO_ADJ:
	emoAdjCnt++;
	broadAdjCnt++;
	strictAdjCnt++;
	break;
      case SEM::NONHUMAN_SHAPE_ADJ:
	nonhumanAdjCnt++;
	shapeAdjCnt++;
	broadAdjCnt++;
	strictAdjCnt++;
	break;
      case SEM::NONHUMAN_COLOR_ADJ:
	nonhumanAdjCnt++;
	colorAdjCnt++;
	broadAdjCnt++;
	strictAdjCnt++;
	break;
      case SEM::NONHUMAN_MATTER_ADJ:
	nonhumanAdjCnt++;
	matterAdjCnt++;
	broadAdjCnt++;
	strictAdjCnt++;
	break;
      case SEM::NONHUMAN_SOUND_ADJ:
	nonhumanAdjCnt++;
	soundAdjCnt++;
	broadAdjCnt++;
	strictAdjCnt++;
	break;
      case SEM::NONHUMAN_OTHER_ADJ:
	nonhumanAdjCnt++;
	nonhumanOtherAdjCnt++;
	broadAdjCnt++;
	strictAdjCnt++;
	break;
      case SEM::TECH_ADJ:
	techAdjCnt++;
	break;
      case SEM::TIME_ADJ:
	timeAdjCnt++;
	broadAdjCnt++;
	break;
      case SEM::PLACE_ADJ:
	placeAdjCnt++;
	broadAdjCnt++;
	break;
      case SEM::SPEC_POS_ADJ:
	specPosAdjCnt++;
	subjectiveAdjCnt++;
	break;
      case SEM::SPEC_NEG_ADJ:
	specNegAdjCnt++;
	subjectiveAdjCnt++;
	break;
      case SEM::POS_ADJ:
	posAdjCnt++;
	subjectiveAdjCnt++;
	break;
      case SEM::NEG_ADJ:
	negAdjCnt++;
	subjectiveAdjCnt++;
	break;
      case SEM::EVALUATIVE_ADJ:
  evaluativeAdjCnt++;
  subjectiveAdjCnt++;
  break;
      case SEM::EPI_POS_ADJ:
	epiPosAdjCnt++;
	subjectiveAdjCnt++;
	break;
      case SEM::EPI_NEG_ADJ:
	epiNegAdjCnt++;
	subjectiveAdjCnt++;
	break;
      case SEM::ABSTRACT_ADJ:
	abstractAdjCnt++;
	break;
      case SEM::ABSTRACT_STATE:
	abstractWwCnt++;
	stateCnt++;
	break;
      case SEM::CONCRETE_STATE:
	concreteWwCnt++;
	stateCnt++;
	break;
      case SEM::UNDEFINED_STATE:
	undefinedWwCnt++;
	stateCnt++;
	break;
      case SEM::ABSTRACT_ACTION:
	abstractWwCnt++;
	actionCnt++;
	break;
      case SEM::CONCRETE_ACTION:
	concreteWwCnt++;
	actionCnt++;
	break;
      case SEM::UNDEFINED_ACTION:
	undefinedWwCnt++;
	actionCnt++;
	break;
      case SEM::ABSTRACT_PROCESS:
	abstractWwCnt++;
	processCnt++;
	break;
      case SEM::CONCRETE_PROCESS:
	concreteWwCnt++;
	processCnt++;
	break;
      case SEM::UNDEFINED_PROCESS:
	undefinedWwCnt++;
	processCnt++;
	break;
      case SEM::ABSTRACT_UNDEFINED:
	abstractWwCnt++;
	break;
      case SEM::CONCRETE_UNDEFINED:
	concreteWwCnt++;
	break;
      case SEM::UNDEFINED_VERB:
	undefinedWwCnt++;
	undefinedATPCnt++;
	break;
      default:
	;
      }
      switch ( ws->intensify_type ) {
          case Intensify::BVNW:
              intensBvnwCnt++;
              intensCnt++;
              break;
          case Intensify::BVBW:
              intensBvbwCnt++;
              intensCnt++;
              break;
          case Intensify::BW:
              intensBwCnt++;
              intensCnt++;
              break;
          case Intensify::COMBI:
              intensCombiCnt++;
              intensCnt++;
              break;
          case Intensify::NW:
              intensNwCnt++;
              intensCnt++;
              break;
          case Intensify::TUSS:
              intensTussCnt++;
              intensCnt++;
              break;
          case Intensify::WW:
              intensWwCnt++;
              intensCnt++;
              break;
          default:
              break;
      }
      // Counts for general nouns
      if (ws->general_noun_type != General::NO_GENERAL) generalNounCnt++;
      if (General::isSeparate(ws->general_noun_type)) generalNounSepCnt++;
      if (General::isRelated(ws->general_noun_type)) generalNounRelCnt++;
      if (General::isActing(ws->general_noun_type)) generalNounActCnt++;
      if (General::isKnowledge(ws->general_noun_type)) generalNounKnowCnt++;
      if (General::isDiscussion(ws->general_noun_type)) generalNounDiscCnt++;
      if (General::isDevelopment(ws->general_noun_type)) generalNounDeveCnt++;

      // Counts for general verbs
      if (ws->general_verb_type != General::NO_GENERAL) generalVerbCnt++;
      if (General::isSeparate(ws->general_verb_type)) generalVerbSepCnt++;
      if (General::isRelated(ws->general_verb_type)) generalVerbRelCnt++;
      if (General::isActing(ws->general_verb_type)) generalVerbActCnt++;
      if (General::isKnowledge(ws->general_verb_type)) generalVerbKnowCnt++;
      if (General::isDiscussion(ws->general_verb_type)) generalVerbDiscCnt++;
      if (General::isDevelopment(ws->general_verb_type)) generalVerbDeveCnt++;

      // Counts for adverbs
      if (ws->adverb_type == Adverb::GENERAL) generalAdverbCnt++;
      if (ws->adverb_type == Adverb::SPECIFIC) specificAdverbCnt++;

      // Counts for compounds
      if (ws->tag == CGN::N) {
        charCntNoun += ws->charCnt;
        word_freq_log_noun += ws->word_freq_log;
        switch (ws->top_freq) {
          case top1000:
            top1000CntNoun++;
          case top2000:
          case top3000:
          case top5000:
            top5000CntNoun++;
          case top10000:
          case top20000:
            top20000CntNoun++;
          default:
            break;
        }

        if (ws->is_compound) {
          compoundCnt++;
          if (ws->compound_parts == 3) {
            compound3Cnt++;
          }

          charCntComp += ws->charCnt;
          charCntHead += ws->charCntHead;
          charCntSat += ws->charCntSat;
          charCntNounCorr += ws->charCntHead;
          charCntCorr += ws->charCntHead;

          word_freq_log_comp += ws->word_freq_log;
          word_freq_log_head += ws->word_freq_log_head;
          word_freq_log_sat += ws->word_freq_log_sat;
          word_freq_log_head_sat += ws->word_freq_log_head_sat;
          word_freq_log_noun_corr += ws->word_freq_log_head;
          word_freq_log_corr += ws->word_freq_log_head;

          switch (ws->top_freq) {
            case top1000:
              top1000CntComp++;
            case top2000:
            case top3000:
            case top5000:
              top5000CntComp++;
            case top10000:
            case top20000:
              top20000CntComp++;
            default:
              break;
          }
          switch (ws->top_freq_head) {
            case top1000:
              top1000CntHead++; top1000CntNounCorr++; top1000CntCorr++;
            case top2000:
            case top3000:
            case top5000:
              top5000CntHead++; top5000CntNounCorr++; top5000CntCorr++;
            case top10000:
            case top20000:
              top20000CntHead++; top20000CntNounCorr++; top20000CntCorr++;
            default:
              break;
          }
          switch (ws->top_freq_sat) {
            case top1000:
              top1000CntSat++;
            case top2000:
            case top3000:
            case top5000:
              top5000CntSat++;
            case top10000:
            case top20000:
              top20000CntSat++;
            default:
              break;
          }
        }
        else {
          charCntNonComp += ws->charCnt;
          charCntNounCorr += ws->charCnt;
          charCntCorr += ws->charCnt;

          word_freq_log_non_comp += ws->word_freq_log;
          word_freq_log_noun_corr += ws->word_freq_log;
          word_freq_log_corr += ws->word_freq_log;

          switch (ws->top_freq) {
            case top1000:
              top1000CntNonComp++; top1000CntNounCorr++; top1000CntCorr++;
            case top2000:
            case top3000:
            case top5000:
              top5000CntNonComp++; top5000CntNounCorr++; top5000CntCorr++;
            case top10000:
            case top20000:
              top20000CntNonComp++; top20000CntNounCorr++; top20000CntCorr++;
            default:
              break;
          }
        }
      }
      else {
        charCntCorr += ws->charCnt;

        if (ws->isContent) {
          word_freq_log_corr += ws->word_freq_log;
        }

        switch (ws->top_freq) {
          case top1000: top1000CntCorr++;
          case top5000: top5000CntCorr++;
          case top20000: top20000CntCorr++;
          default: break;
        }
      }

      sv.push_back( ws );
    }
  }
  if ( alpDoc ){
    xmlFreeDoc( alpDoc );
  }
  al_gem = getMeanAL();
  al_max = getHighestAL();
  resolveConnectives();
  resolveSituations();
  calculate_MTLDs();
  if ( settings.doLsa ){
    resolveLSA( LSAword_dists );
  }
  resolveMultiWordIntensify();
  // Disabled for now
  //  resolveMultiWordAfks();
  resolvePrepExpr();
  if ( question )
    questCnt = 1;
  if ( (morphNegCnt + propNegCnt) > 1 )
    multiNegCnt = 1;
  if ( word_freq == 0 || contentCnt == 0 )
    word_freq_log = NAN;
  else
    word_freq_log = word_freq / contentCnt;
  if ( lemma_freq == 0 || contentCnt == 0 )
    lemma_freq_log = NAN;
  else
    lemma_freq_log = lemma_freq / contentCnt;
  if ( contentCnt == nameCnt || word_freq_n == 0 )
    word_freq_log_n = NAN;
  else
    word_freq_log_n = word_freq_n / (contentCnt-nameCnt);
  if ( contentCnt == nameCnt || lemma_freq_n == 0 )
    lemma_freq_log_n = NAN;
  else
    lemma_freq_log_n = lemma_freq_n / (contentCnt-nameCnt);
  np_length( s, npCnt, indefNpCnt, npSize );
}

void sentStats::addMetrics( ) const {
  structStats::addMetrics( );
  folia::FoliaElement *el = folia_node;
  folia::Document *doc = el->doc();
  if ( passiveCnt > 0 )
    addOneMetric( doc, el, "isPassive", "true" );
  if ( questCnt > 0 )
    addOneMetric( doc, el, "isQuestion", "true" );
  if ( impCnt > 0 )
    addOneMetric( doc, el, "isImperative", "true" );
}

parStats::parStats( int index,
		    folia::Paragraph *p,
		    const map<string,double>& LSA_word_dists,
		    const map<string,double>& LSA_sent_dists ):
  structStats( index, p, "par" )
{
  sentCnt = 0;
  vector<folia::Sentence*> sents = p->sentences();
  sentStats *prev = 0;
  for ( size_t i=0; i < sents.size(); ++i ){
    sentStats *ss = new sentStats( i, sents[i], prev, LSA_word_dists );
    prev = ss;
    merge( ss );
  }
  if ( settings.doLsa ){
    resolveLSA( LSA_sent_dists );
  }
  calculate_MTLDs();
  if ( word_freq == 0 || contentCnt == 0 )
    word_freq_log = NAN;
  else
    word_freq_log = word_freq / contentCnt;
  if ( contentCnt == nameCnt || word_freq_n == 0 )
    word_freq_log_n = NAN;
  else
    word_freq_log_n = word_freq_n / (contentCnt-nameCnt);
  if ( lemma_freq == 0 || contentCnt == 0 )
    lemma_freq_log = NAN;
  else
    lemma_freq_log = lemma_freq / contentCnt;
  if ( contentCnt == nameCnt || lemma_freq_n == 0 )
    lemma_freq_log_n = NAN;
  else
    lemma_freq_log_n = lemma_freq_n / (contentCnt-nameCnt);
}


void parStats::addMetrics( ) const {
  folia::FoliaElement *el = folia_node;
  structStats::addMetrics( );
  addOneMetric( el->doc(), el,
		"sentence_count", toString(sentCnt) );
}

void parStats::setLSAvalues( double suc, double net, double ctx ){
  if ( suc > 0 )
    lsa_sent_suc = suc;
  if ( net > 0 )
    lsa_sent_net = net;
  if ( ctx > 0 )
    lsa_sent_ctx = ctx;
}

void docStats::setLSAvalues( double suc, double net, double ctx ){
  if ( suc > 0 )
    lsa_par_suc = suc;
  if ( net > 0 )
    lsa_par_net = net;
  if ( ctx > 0 )
    lsa_par_ctx = ctx;
}

//#define DEBUG_DOL

void docStats::calculate_doc_overlap( ){
  vector<const wordStats*> wv2 = collectWords();
  if ( wv2.size() < settings.overlapSize )
    return;
  vector<string> wordbuffer;
  vector<string> lemmabuffer;
  for ( vector<const wordStats*>::const_iterator it = wv2.begin();
	it != wv2.end();
	++it ){
    if ( (*it)->wordProperty() == CGN::ISLET )
      continue;
    string l_word =  (*it)->ltext();
    string l_lemma = (*it)->llemma();
    if ( wordbuffer.size() >= settings.overlapSize ){
#ifdef DEBUG_DOL
      cerr << "Document overlap" << endl;
      cerr << "wordbuffer= " << wordbuffer << endl;
      cerr << "lemmabuffer= " << lemmabuffer << endl;
      cerr << "test overlap: << " << l_word << " " << l_lemma << endl;
#endif
      if ( (*it)->isOverlapCandidate() ){
#ifdef DEBUG_DOL
	int tmp = doc_word_overlapCnt;
#endif
	argument_overlap( l_word, wordbuffer, doc_word_overlapCnt );
#ifdef DEBUG_DOL
	if ( doc_word_overlapCnt > tmp ){
	  cerr << "word OVERLAP " << l_word << endl;
	}
#endif
#ifdef DEBUG_DOL
	tmp = doc_lemma_overlapCnt;
#endif
	argument_overlap( l_lemma, lemmabuffer, doc_lemma_overlapCnt );
#ifdef DEBUG_DOL
	if ( doc_lemma_overlapCnt > tmp ){
	  cerr << "lemma OVERLAP " << l_lemma << endl;
	}
#endif
      }
#ifdef DEBUG_DOL
      else {
	cerr << "geen kandidaat" << endl;
      }
#endif
      wordbuffer.erase(wordbuffer.begin());
      lemmabuffer.erase(lemmabuffer.begin());
    }
    wordbuffer.push_back( l_word );
    lemmabuffer.push_back( l_lemma );
  }
}


//#define DEBUG_LSA_SERVER

void docStats::gather_LSA_word_info( folia::Document *doc ){
  string host = config.lookUp( "host", "lsa_words" );
  string port = config.lookUp( "port", "lsa_words" );
  Sockets::ClientSocket client;
  if ( !client.connect( host, port ) ){
    cerr << "failed to open LSA connection: "<< host << ":" << port << endl;
    cerr << "Reason: " << client.getMessage() << endl;
    exit( EXIT_FAILURE );
  }
  vector<folia::Word*> wv = doc->words();
  set<string> bow;
  for ( size_t i=0; i < wv.size(); ++i ){
    UnicodeString us = wv[i]->text();
    us.toLower();
    string s = folia::UnicodeToUTF8( us );
    bow.insert(s);
  }
  while ( !bow.empty() ){
    set<string>::iterator it = bow.begin();
    string word = *it;
    bow.erase( it );
    it = bow.begin();
    while ( it != bow.end() ) {
      string call = word + "\t" + *it;
      string rcall = *it + "\t" + word;
      if ( LSA_word_dists.find( call ) == LSA_word_dists.end() ){
#ifdef DEBUG_LSA_SERVER
	cerr << "calling LSA: '" << call << "'" << endl;
#endif
	client.write( call + "\r\n" );
	string s;
	if ( !client.read(s) ){
	  cerr << "LSA connection failed " << endl;
	  exit( EXIT_FAILURE );
	}
#ifdef DEBUG_LSA_SERVER
	cerr << "received data [" << s << "]" << endl;
#endif
	double result = 0;
	if ( !stringTo( s , result ) ){
	  cerr << "LSA result conversion failed: " << s << endl;
	  result = 0;
	}
#ifdef DEBUG_LSA_SERVER
	cerr << "LSA result: " << result << endl;
#endif
#ifdef DEBUG_LSA
	cerr << "LSA: '" << call << "' ==> " << result << endl;
#endif
	if ( result != 0 ){
	  LSA_word_dists[call] = result;
	  LSA_word_dists[rcall] = result;
	}
      }
      ++it;
    }
  }
}

void docStats::gather_LSA_doc_info( folia::Document *doc ){
  string host = config.lookUp( "host", "lsa_docs" );
  string port = config.lookUp( "port", "lsa_docs" );
  Sockets::ClientSocket client;
  if ( !client.connect( host, port ) ){
    cerr << "failed to open LSA connection: "<< host << ":" << port << endl;
    cerr << "Reason: " << client.getMessage() << endl;
    exit( EXIT_FAILURE );
  }
  vector<folia::Paragraph*> pv = doc->paragraphs();
  map<string,string> norm_pv;
  map<string,string> norm_sv;
  for ( size_t p=0; p < pv.size(); ++p ){
    vector<folia::Sentence*> sv = pv[p]->sentences();
    string norm_p;
    for ( size_t s=0; s < sv.size(); ++s ){
      vector<folia::Word*> wv = sv[s]->words();
      set<string> bow;
      for ( size_t i=0; i < wv.size(); ++i ){
	UnicodeString us = wv[i]->text();
	us.toLower();
	string s = folia::UnicodeToUTF8( us );
	bow.insert(s);
      }
      string norm_s;
      set<string>::iterator it = bow.begin();
      while ( it != bow.end() ) {
	norm_s += *it++ + " ";
      }
      norm_sv[sv[s]->id()] = norm_s;
      norm_p += norm_s;
    }
    norm_pv[pv[p]->id()] = norm_p;
  }
#ifdef DEBUG_LSA_SERVER
  cerr << "LSA doc Paragraaf results" << endl;
  for ( map<string,string>::const_iterator it = norm_pv.begin();
	it != norm_pv.end();
	++it ){
    cerr << "paragraaf " << it->first << endl;
    cerr << it->second << endl;
  }
  cerr << "en de Zinnen" << endl;
  for ( map<string,string>::const_iterator it = norm_sv.begin();
	it != norm_sv.end();
	++it ){
    cerr << "zin " <<  it->first << endl;
    cerr << it->second << endl;
  }
#endif

  for ( map<string,string>::const_iterator it1 = norm_pv.begin();
	it1 != norm_pv.end();
	++it1 ){
    for ( map<string,string>::const_iterator it2 = it1;
	  it2 != norm_pv.end();
	  ++it2 ){
      if ( it2 == it1 )
	continue;
      string index = it1->first + "<==>" + it2->first;
      string rindex = it2->first + "<==>" + it1->first;
#ifdef DEBUG_LSA
      cerr << "LSA combine paragraaf " << index << endl;
#endif
      string call = it1->second + "\t" + it2->second;
      if ( LSA_paragraph_dists.find( index ) == LSA_paragraph_dists.end() ){
#ifdef DEBUG_LSA_SERVER
	cerr << "calling LSA docs: '" << call << "'" << endl;
#endif
	client.write( call + "\r\n" );
	string s;
	if ( !client.read(s) ){
	  cerr << "LSA connection failed " << endl;
	  exit( EXIT_FAILURE );
	}
#ifdef DEBUG_LSA_SERVER
	cerr << "received data [" << s << "]" << endl;
#endif
	double result = 0;
	if ( !stringTo( s , result ) ){
	  cerr << "LSA result conversion failed: " << s << endl;
	  result = 0;
	}
#ifdef DEBUG_LSA_SERVER
	cerr << "LSA result: " << result << endl;
#endif
#ifdef DEBUG_LSA
	cerr << "LSA: '" << index << "' ==> " << result << endl;
#endif
	if ( result != 0 ){
	  LSA_paragraph_dists[index] = result;
	  LSA_paragraph_dists[rindex] = result;
	}
      }
    }
  }
  for ( map<string,string>::const_iterator it1 = norm_sv.begin();
	it1 != norm_sv.end();
	++it1 ){
    for ( map<string,string>::const_iterator it2 = it1;
	  it2 != norm_sv.end();
	  ++it2 ){
      if ( it2 == it1 )
	continue;
      string index = it1->first + "<==>" + it2->first;
      string rindex = it2->first + "<==>" + it1->first;
#ifdef DEBUG_LSA
      cerr << "LSA combine sentence " << index << endl;
#endif
      string call = it1->second + "\t" + it2->second;
      if ( LSA_sentence_dists.find( index ) == LSA_sentence_dists.end() ){
#ifdef DEBUG_LSA_SERVER
	cerr << "calling LSA docs: '" << call << "'" << endl;
#endif
	client.write( call + "\r\n" );
	string s;
	if ( !client.read(s) ){
	  cerr << "LSA connection failed " << endl;
	  exit( EXIT_FAILURE );
	}
#ifdef DEBUG_LSA_SERVER
	cerr << "received data [" << s << "]" << endl;
#endif
	double result = 0;
	if ( !stringTo( s , result ) ){
	  cerr << "LSA result conversion failed: " << s << endl;
	  result = 0;
	}
#ifdef DEBUG_LSA_SERVER
	cerr << "LSA result: " << result << endl;
#endif
#ifdef DEBUG_LSA
	cerr << "LSA: '" << index << "' ==> " << result << endl;
#endif
	if ( result != 0 ){
	  LSA_sentence_dists[index] = result;
	  LSA_sentence_dists[rindex] = result;
	}
      }
    }
  }
}

docStats::docStats( folia::Document *doc ):
  structStats( 0, 0, "document" ),
  doc_word_overlapCnt(0), doc_lemma_overlapCnt(0)
{
  sentCnt = 0;
  doc->declare( folia::AnnotationType::METRIC,
		"metricset",
		"annotator='tscan'" );
  doc->declare( folia::AnnotationType::POS,
		"tscan-set",
		"annotator='tscan'" );
  if ( !settings.style.empty() ){
    doc->replaceStyle( "text/xsl", settings.style );
  }
  if ( settings.doLsa ){
    gather_LSA_word_info( doc );
    gather_LSA_doc_info( doc );
  }
  vector<folia::Paragraph*> pars = doc->paragraphs();
  if ( pars.size() > 0 )
    folia_node = pars[0]->parent();
  for ( size_t i=0; i != pars.size(); ++i ){
    parStats *ps = new parStats( i, pars[i], LSA_word_dists, LSA_sentence_dists );
      merge( ps );
  }
  if ( settings.doLsa ){
    resolveLSA( LSA_paragraph_dists );
  }
  calculate_MTLDs();
  if ( word_freq == 0 || contentCnt == 0 )
    word_freq_log = NAN;
  else
    word_freq_log = word_freq / contentCnt;
  if ( contentCnt == nameCnt || word_freq_n == 0 )
    word_freq_log_n = NAN;
  else
    word_freq_log_n = word_freq_n / (contentCnt-nameCnt);
  if ( lemma_freq == 0 || contentCnt == 0 )
    lemma_freq_log = NAN;
  else
    lemma_freq_log = lemma_freq / contentCnt;
  if ( contentCnt == nameCnt || lemma_freq_n == 0 )
    lemma_freq_log_n = NAN;
  else
    lemma_freq_log_n = lemma_freq_n / (contentCnt-nameCnt);
  calculate_doc_overlap( );

}

string docStats::rarity( int level ) const {
  map<string,int>::const_iterator it = unique_lemmas.begin();
  int rare = 0;
  while ( it != unique_lemmas.end() ){
    if ( it->second <= level )
      ++rare;
    ++it;
  }
  double result = rare/double( unique_lemmas.size() );
  return toString( result );
}

void docStats::addMetrics() const {
  folia::FoliaElement *el = folia_node;
  structStats::addMetrics();
  addOneMetric( el->doc(), el,
		"sentence_count", toString( sentCnt ) );
  addOneMetric( el->doc(), el,
		"paragraph_count", toString( sv.size() ) );
  addOneMetric( el->doc(), el,
		"word_ttr", toString( unique_words.size()/double(wordCnt) ) );
  addOneMetric( el->doc(), el,
		"word_mtld", toString( word_mtld ) );
  addOneMetric( el->doc(), el,
		"lemma_ttr", toString( unique_lemmas.size()/double(wordCnt) ) );
  addOneMetric( el->doc(), el,
		"lemma_mtld", toString( lemma_mtld ) );
  if ( nameCnt != 0 ){
    addOneMetric( el->doc(), el,
		  "names_ttr", toString( unique_names.size()/double(nameCnt) ) );
  }
  addOneMetric( el->doc(), el,
		"name_mtld", toString( name_mtld ) );

  if ( contentCnt != 0 ){
    addOneMetric( el->doc(), el,
		  "content_word_ttr", toString( unique_contents.size()/double(contentCnt) ) );
  }
  addOneMetric( el->doc(), el,
		"content_mtld", toString( content_mtld ) );

  if ( timeSitCnt != 0 ){
    addOneMetric( el->doc(), el,
		  "time_sit_ttr", toString( unique_tijd_sits.size()/double(timeSitCnt) ) );
  }
  addOneMetric( el->doc(), el,
		"tijd_sit_mtld", toString( tijd_sit_mtld ) );

  if ( spaceSitCnt != 0 ){
    addOneMetric( el->doc(), el,
		  "space_sit_ttr", toString( unique_ruimte_sits.size()/double(spaceSitCnt) ) );
  }
  addOneMetric( el->doc(), el,
		"ruimte_sit_mtld", toString( ruimte_sit_mtld ) );

  if ( causeSitCnt != 0 ){
    addOneMetric( el->doc(), el,
		  "cause_sit_ttr", toString( unique_cause_sits.size()/double(causeSitCnt) ) );
  }
  addOneMetric( el->doc(), el,
		"cause_sit_mtld", toString( cause_sit_mtld ) );

  if ( emoSitCnt != 0 ){
    addOneMetric( el->doc(), el,
		  "emotion_sit_ttr", toString( unique_emotion_sits.size()/double(emoSitCnt) ) );
  }
  addOneMetric( el->doc(), el,
		"emotion_sit_mtld", toString( emotion_sit_mtld ) );

  if ( tempConnCnt != 0 ){
    addOneMetric( el->doc(), el,
		  "temp_conn_ttr", toString( unique_temp_conn.size()/double(tempConnCnt) ) );
  }
  addOneMetric( el->doc(), el,
		"temp_conn_mtld", toString(temp_conn_mtld) );

  if ( opsomWgConnCnt != 0 ){
    addOneMetric( el->doc(), el,
		  "opsom_wg_conn_ttr", toString( unique_reeks_wg_conn.size()/double(opsomWgConnCnt) ) );
  }
  addOneMetric( el->doc(), el,
		"opsom_wg_conn_mtld", toString(reeks_wg_conn_mtld) );

  if ( opsomZinConnCnt != 0 ){
    addOneMetric( el->doc(), el,
      "opsom_zin_conn_ttr", toString( unique_reeks_zin_conn.size()/double(opsomZinConnCnt) ) );
  }
  addOneMetric( el->doc(), el,
    "opsom_zin_conn_mtld", toString(reeks_zin_conn_mtld) );

  if ( contrastConnCnt != 0 ){
    addOneMetric( el->doc(), el,
		  "contrast_conn_ttr", toString( unique_contr_conn.size()/double(contrastConnCnt) ) );
  }
  addOneMetric( el->doc(), el,
		"contrast_conn_mtld", toString(contr_conn_mtld) );

  if ( compConnCnt != 0 ){
    addOneMetric( el->doc(), el,
		  "comp_conn_ttr", toString( unique_comp_conn.size()/double(compConnCnt) ) );
  }
  addOneMetric( el->doc(), el,
		"comp_conn_mtld", toString(comp_conn_mtld) );


  if ( causeConnCnt != 0 ){
    addOneMetric( el->doc(), el,
		  "cause_conn_ttr", toString( unique_cause_conn.size()/double(causeConnCnt) ) );
  }
  addOneMetric( el->doc(), el,
		"cause_conn_mtld", toString(cause_conn_mtld) );


  addOneMetric( el->doc(), el,
		"rar_index", rarity( settings.rarityLevel ) );
  addOneMetric( el->doc(), el,
		"document_word_argument_overlap_count", toString( doc_word_overlapCnt ) );
  addOneMetric( el->doc(), el,
		"document_lemma_argument_overlap_count", toString( doc_lemma_overlapCnt ) );
}

void docStats::toCSV( const string& name, csvKind what ) const {
  if ( what == DOC_CSV ){
    string fname = name + ".document.csv";
    ofstream out( fname.c_str() );
    if ( out ){
      // 20141003: New features: paragraphs/sentences/words per document
      CSVheader( out, "Inputfile,Par_per_doc,Zin_per_doc,Word_per_doc" );
      out << name << "," << sv.size() << ",";
      structStats::toCSV( out );
      cerr << "stored document statistics in " << fname << endl;
    }
    else {
      cerr << "storing document statistics in " << fname << " FAILED!" << endl;
    }
  }
  else if ( what == PAR_CSV ){
    string fname = name + ".paragraphs.csv";
    ofstream out( fname.c_str() );
    if ( out ){
      for ( size_t par=0; par < sv.size(); ++par ){
	if ( par == 0 )
    // 20141003: New features: sentences/words per paragraph
	  sv[0]->CSVheader( out, "Inputfile,Segment,Zin_per_par,Wrd_per_par" );
	out << name << "," << sv[par]->id << ",";
	sv[par]->toCSV( out );
    }
      cerr << "stored paragraph statistics in " << fname << endl;
    }
    else {
      cerr << "storing paragraph statistics in " << fname << " FAILED!" << endl;
    }
  }
  else if ( what == SENT_CSV ){
    string fname = name + ".sentences.csv";
    ofstream out( fname.c_str() );
    if ( out ){
      for ( size_t par=0; par < sv.size(); ++par ){
	for ( size_t sent=0; sent < sv[par]->sv.size(); ++sent ){
	  if ( par == 0 && sent == 0 )
	    sv[0]->sv[0]->CSVheader( out, "Inputfile,Segment,Getokeniseerde_zin" );
	  out << name << "," << sv[par]->sv[sent]->id << ",";
	  sv[par]->sv[sent]->toCSV( out );
	}
      }
      cerr << "stored sentence statistics in " << fname << endl;
    }
    else {
      cerr << "storing sentence statistics in " << fname << " FAILED!" << endl;
    }
  }
  else if ( what == WORD_CSV ){
    string fname = name + ".words.csv";
    ofstream out( fname.c_str() );
    if ( out ){
      for ( size_t par=0; par < sv.size(); ++par ){
	for ( size_t sent=0; sent < sv[par]->sv.size(); ++sent ){
	  for ( size_t word=0; word < sv[par]->sv[sent]->sv.size(); ++word ){
	    if ( par == 0 && sent == 0 && word == 0 )
	      sv[0]->sv[0]->sv[0]->CSVheader( out );
	    out << name << ",";
	    sv[par]->sv[sent]->sv[word]->toCSV( out );
	  }
	}
      }
      cerr << "stored word statistics in " << fname << endl;
    }
    else {
      cerr << "storing word statistics in " << fname << " FAILED!" << endl;
    }
  }
}

//#define DEBUG_FROG

folia::Document *getFrogResult( istream& is ){
  string host = config.lookUp( "host", "frog" );
  string port = config.lookUp( "port", "frog" );
  Sockets::ClientSocket client;
  if ( !client.connect( host, port ) ){
    cerr << "failed to open Frog connection: "<< host << ":" << port << endl;
    cerr << "Reason: " << client.getMessage() << endl;
    return 0;
  }
#ifdef DEBUG_FROG
  cerr << "start input loop" << endl;
#endif
  bool incomment = false;
  string line;
  while ( safe_getline( is, line ) ){
#ifdef DEBUG_FROG
    cerr << "read: '" << line << "'" << endl;
#endif
    if ( line.length() > 2 ){
      string start = line.substr(0,3);
      if ( start == "###" )
	continue;
      else if ( start == "<<<" ){
	if ( incomment ){
	  cerr << "Nested comment (<<<) not allowed!" << endl;
	  return 0;
	}
	else {
	  incomment = true;
	}
      }
      else if ( start == ">>>" ){
	if ( !incomment ){
	  cerr << "end of comment (>>>) found without start." << endl;
	  return 0;
	}
	else {
	  incomment = false;
	  continue;
	}
      }
    }
    if ( incomment )
      continue;
    if ( settings.sentencePerLine ) {
      client.write( line + "\n\n" );
    }
    else {
      client.write( line + "\n" );
    }
  }
  client.write( "\nEOT\n" );
  string result;
  string s;
  while ( client.read(s) ){
    if ( s == "READY" )
      break;
    result += s + "\n";
  }
#ifdef DEBUG_FROG
  cerr << "received data [" << result << "]" << endl;
#endif
  folia::Document *doc = 0;
  if ( !result.empty() && result.size() > 10 ){
#ifdef DEBUG_FROG
    cerr << "start FoLiA parsing" << endl;
#endif
    doc = new folia::Document();
    try {
      doc->readFromString( result );
#ifdef DEBUG_FROG
      cerr << "finished" << endl;
#endif
    }
    catch ( std::exception& e ){
      cerr << "FoLiaParsing failed:" << endl
	   << e.what() << endl;
    }
  }
  return doc;
}

//#define DEBUG_ALPINO

xmlDoc *AlpinoServerParse( folia::Sentence *sent ){
  string host = config.lookUp( "host", "alpino" );
  string port = config.lookUp( "port", "alpino" );
  Sockets::ClientSocket client;
  if ( !client.connect( host, port ) ){
    cerr << "failed to open Alpino connection: "<< host << ":" << port << endl;
    cerr << "Reason: " << client.getMessage() << endl;
    exit( EXIT_FAILURE );
  }
#ifdef DEBUG_ALPINO
  cerr << "start input loop" << endl;
#endif
  string txt = folia::UnicodeToUTF8(sent->toktext());
  client.write( txt + "\n\n" );
  string result;
  string s;
  while ( client.read(s) ){
    result += s + "\n";
  }
#ifdef DEBUG_ALPINO
  cerr << "received data [" << result << "]" << endl;
#endif
  xmlDoc *doc = xmlReadMemory( result.c_str(), result.length(),
			       0, 0, XML_PARSE_NOBLANKS );
  string txtfile = workdir_name + "1.xml";
  xmlSaveFormatFileEnc( txtfile.c_str(), doc, "UTF8", 1 );
  return doc;
}

int main(int argc, char *argv[]) {
  struct stat sbuf;
  pid_t pid = getpid();
  workdir_name = "/tmp/tscan-" + toString( pid ) + "/";
  int res = stat( workdir_name.c_str(), &sbuf );
  if ( res == -1 || !S_ISDIR(sbuf.st_mode) ){
    res = mkdir( workdir_name.c_str(), S_IRWXU|S_IRWXG );
    if ( res ){
      cerr << "problem creating working dir '" << workdir_name
	   << "' : " << res << endl;
      exit( EXIT_FAILURE );
    }
  }
  cerr << "TScan " << VERSION << endl;
  cerr << "working dir " << workdir_name << endl;
  string shortOpt = "ht:o:Vn";
  string longOpt = "threads:,config:,skip:,version";
  TiCC::CL_Options opts( shortOpt, longOpt );
  try {
    opts.init( argc, argv );
  }
  catch( TiCC::OptionError& e ){
    cerr << e.what() << endl;
    usage();
    exit( EXIT_SUCCESS );
  }

  if ( opts.extract( 'h' ) ||
       opts.extract( "help" ) ){
    usage();
    exit( EXIT_SUCCESS );
  }

  if ( opts.extract( 'V' ) ||
       opts.extract( "version" ) ){
    exit( EXIT_SUCCESS );
  }

  string t_option;
  opts.extract( 't', t_option );
  vector<string> inputnames;
  if ( t_option.empty() ){
    inputnames = opts.getMassOpts();
  }
  else {
    inputnames = TiCC::searchFiles( t_option );
  }

  if ( inputnames.size() == 0 ){
    cerr << "no input file(s) found" << endl;
    exit(EXIT_FAILURE);
  }
  string o_option;
  if ( opts.extract( 'o', o_option ) ){
    if ( inputnames.size() > 1 ){
      cerr << "-o option not supported for multiple input files" << endl;
      exit(EXIT_FAILURE);
    }
  }

  string val;
  if ( opts.extract( "threads", val ) ){
#ifdef HAVE_OPENMP
    int num = TiCC::stringTo<int>( val );
    if ( num < 1 || num > 4 ){
      cerr << "wrong value for 'threads' option. (must be >=1 and <= 4 )"
	   << endl;
      exit(EXIT_FAILURE);
    }
    else {
      omp_set_num_threads( num );
    }
#else
    cerr << "No OPEN_MP support available. 'threads' option ignored." << endl;
#endif
  }

  opts.extract( "config", configFile );
  if ( !configFile.empty() &&
       config.fill( configFile ) ){
    settings.init( config );
  }
  else {
    cerr << "invalid configuration" << endl;
    exit( EXIT_FAILURE );
  }
  if ( settings.showProblems ){
    problemFile.open( "problems.log" );
    problemFile << "missing,word,lemma,voll_lemma" << endl;
  }
  if ( opts.extract( 'n' ) ) {
    settings.sentencePerLine = true;
  }
  if ( opts.extract( "skip", val ) ) {
    string skip = val;
    if ( skip.find_first_of("wW") != string::npos ){
      settings.doWopr = false;
    }
    if ( skip.find_first_of("lL") != string::npos ){
      settings.doLsa = false;
    }
    if ( skip.find_first_of("aA") != string::npos ){
      settings.doAlpino = false;
      settings.doAlpinoServer = false;
    }
    if ( skip.find_first_of("cC") != string::npos ){
      settings.doXfiles = false;
    }
  };
  if ( !opts.empty() ){
    cerr << "unsupported options in command: " << opts.toString() << endl;
    exit(EXIT_FAILURE);
  }

  if ( inputnames.size() > 1 ){
    cerr << "processing " << inputnames.size() << " files." << endl;
  }
  for ( size_t i = 0; i < inputnames.size(); ++i ){
    string inName = inputnames[i];
    string outName;
    if ( !o_option.empty() ){
      // just 1 inputfile
      outName = o_option;
    }
    else {
      outName = inName + ".tscan.xml";
    }
    ifstream is( inName.c_str() );
    if ( !is ){
      cerr << "failed to open file '" << inName << "'" << endl;
      continue;
    }
    else {
      cerr << "opened file " <<  inName << endl;
      folia::Document *doc = getFrogResult( is );
      if ( !doc ){
	cerr << "big trouble: no FoLiA document created " << endl;
	continue;
      }
      else {
	docStats analyse( doc );
	analyse.addMetrics(); // add metrics info to doc
	doc->save( outName );
	if ( settings.doXfiles ){
	  analyse.toCSV( inName, DOC_CSV );
	  analyse.toCSV( inName, PAR_CSV );
	  analyse.toCSV( inName, SENT_CSV );
	  analyse.toCSV( inName, WORD_CSV );
	}
	delete doc;
	cerr << "saved output in " << outName << endl;
      }
    }
  }
  exit(EXIT_SUCCESS);
}
