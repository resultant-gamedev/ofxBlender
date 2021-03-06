#include "File.h"
#include <fstream>
#include "Parser.h"
#include "Poco/InflatingStream.h"
#include "Poco/StreamCopier.h"
#include "Poco/TemporaryFile.h"
#include "Poco/Exception.h"

class file;

namespace ofx {
namespace blender {

//helpers to navigate within the file
void align(ifstream& stream) {
	streamoff pos = stream.tellg();
	streamoff trim = pos % 4;
	if(trim != 0)
		stream.seekg(pos + 4 - trim);
}

///////////////////////////////// PARSER HELPER


////////////////////

File::File() {
	scale = 10;
	skipTextures = false;
	Parser::init();
}

File::~File() {
}

void File::seek(streamoff to) {
	file.seekg(to);
}

char* File::readChar(streamsize length) {
	char* data = new char[length];
	file.read(data, length);
	return data;
}

std::string File::readString(streamsize length) {
	if(length == 0) { //0 = read until terminated with zero
		string res = "";
		char cur = readChar(1)[0];
		while(cur != '\0') {
			res += cur;
			cur = readChar(1)[0];
		}
		return trim(res);
	} else {
		string ret = string(readChar(length), length);
		return trim(ret);
	}
	return "";
}

void File::readHeader(File::Block& block) {
	block.code = readString(4);

	//clean the code of potential 0s at end
	string cleanCode = "";
	for(char c: block.code) {
		if(c != 0)
			cleanCode += c;
	}
	block.code = cleanCode;

	if(block.code != "ENDB") {
		block.size = read<unsigned int>();
		block.address = readPointer();
		block.SDNAIndex = read<unsigned int>();
		block.count = read<unsigned int>();
		block.offset = file.tellg();
	} else {
		block.size = read<unsigned int>();
		block.address = 0;
		block.SDNAIndex = 0;
		block.count = 0;
		block.offset = file.tellg();
	}
}

bool File::load(string path) {
	//load the blend file into the stream
	if(file.is_open())
		file.close();
	file.open(ofToDataPath(path, true).c_str(), ios::binary);
	//info should contain blender now, if not it is compressed
	string info = readString(7);

	//check if the file is gzipped
	if(info != "BLENDER") {
		seek(0);

		//unzip the blend file to a temp file and reload
		Poco::InflatingInputStream inflater(file, Poco::InflatingStreamBuf::STREAM_GZIP);

		Poco::TemporaryFile tempFile;
		tempFile.keepUntilExit();
		std::ofstream out(tempFile.path().c_str(), ios::binary);
		Poco::StreamCopier::copyStream( inflater, out);
		out.close();

		file.close();
		file.open(tempFile.path().c_str(), ios::binary);
		info = readString(7);

		if(info != "BLENDER") {
			ofLogWarning(OFX_BLENDER) << "Could not read blend file " << path;
		} else {
			ofLogVerbose(OFX_BLENDER) << "Blend file is gzipped, temporarily decompressed contents to " << tempFile.path();
		}
	}

	//now extract the rest of the header data
	string tempString = readString(1);

	if(tempString == "-")
		pointerSize = 8;
	else if(tempString == "_")
		pointerSize = 4;
	//ofLogVerbose(OFX_BLENDER) << "Pointer Size is " << pointerSize;

	if(pointerSize == 4) {
		readPointer = std::bind(&File::read<unsigned int>, this);
	} else {
		readPointer = std::bind(&File::read<unsigned long>, this);
	}

	bool littleEndianness;
	char structPre = ' ';
	tempString = readString(1);
	if(tempString == "v") {
		littleEndianness = true;
		structPre = '<';
	} else if(tempString == "V") {
		littleEndianness = false;
		structPre = '>';
	}
	//ofLogVerbose(OFX_BLENDER) << "Struct pre is " << structPre;
	//ofLogVerbose(OFX_BLENDER) << "Little Endianness is " << littleEndianness;

	//version
	version = readString(3);

	//now go through all them blocks
	blocks.push_back(Block(this));

	//read the first block
	readHeader(blocks.back());

	while(blocks.back().code != "DNA1" && blocks.back().code != "SDNA") {

		//skip the block data
		file.seekg(file.tellg() + streamoff(blocks.back().size));

		//read a new block
		blocks.push_back(Block(this));
		readHeader(blocks.back());
	}

	//advance
	readString(4);
	readString(4);

	//NAMES
	unsigned int numNames = read<unsigned int>();
	for(unsigned int i=0; i<numNames; i++) {
		catalog.names.push_back(DNAName(readString(0)));
	}
	align(file);

	//TYPES
	readString(4);
	unsigned int numTypes = read<unsigned int>();
	//cout << "FOUND TYPES " << numTypes << endl;
	for(unsigned int i=0; i<numTypes; i++) {
		catalog.types.push_back(DNAType(readString(0), i));
	}
	align(file);

	//TYPE LENGTHS
	readString(4);;
	for(unsigned int i=0; i<numTypes; i++) {
		catalog.types[i].size = read<unsigned short>();
		if(catalog.types[i].size == 0) //assume it is a pointer
			catalog.types[i].size = pointerSize;
	}
	align(file);

	//STRUCTURES
	readString(4);
	unsigned int numStructs = read<unsigned int>();
	//cout << "FOUND STRUCTURES " << numStructs << endl;
	for(unsigned int i=0; i<numStructs; i++) {
		//get the type
		unsigned int index = read<unsigned short>();
		DNAType* type = &catalog.types[index];
		catalog.structures.push_back(DNAStructure(type));
		DNAStructure& structure = catalog.structures.back();

		//get the fields for the structure
		unsigned short numFields = read<unsigned short>();
		unsigned int curOffset = 0;
		for(unsigned int j=0; j<numFields; j++) {
			unsigned short typeIndex = read<unsigned short>();
			unsigned short nameIndex = read<unsigned short>();
			DNAType* type = &catalog.types[typeIndex];
			DNAName* name = &catalog.names[nameIndex];
			structure.fields.push_back(DNAField(type, name, curOffset));

			//if the field is a pointer, then only add the pointer size to offset
			bool offsetSet = false;
			if(structure.fields.back().isPointer) {
				int amount = 0;
				if(structure.fields.back().isArray) {
					amount = structure.fields.back().arraySizes[0];
				}
				if(amount == 0)
					amount = 1;
				curOffset += (pointerSize * amount);
				offsetSet = true;
			} else if(structure.fields.back().isArray) { //arrays add n times the size to offset
				float multi = 0;
				for(int s: structure.fields.back().arraySizes) {
					if(s!=-1) {
						if(multi == 0)
							multi += s;
						else
							multi *= s;
					}
				}

				if(multi != 0)
					offsetSet = true;

				curOffset += type->size * multi;
			}
			if(!offsetSet) {
				curOffset += type->size;
			}
		}
	}
	align(file);

	//now link all structures with the File Blocks
	vector<Block>::iterator it = blocks.begin();
	while(it != blocks.end()) {
		(*it).structure = &catalog.structures[(*it).SDNAIndex];
		it++;
	}

	ofLogVerbose(OFX_BLENDER) << "Loaded \"" << path << "\" - Blender version is " <<  version;

	return true;
}

//////////////////////////////////////////////////////////////////////////////////////

void* File::parseFileBlock(Block* block) {
	//check loopkup table
	if(block == NULL)
		return NULL;
	if(parsedBlocks.find(block->address) == parsedBlocks.end()) {
		parsedBlocks[block->address] = Parser::parseFileBlock(block);
	}
	return parsedBlocks[block->address];
}

void* File::parseFileBlock(unsigned long adress) {
	return parseFileBlock(getBlockByAddress(adress));
}

unsigned int File::getNumberOfTypes(string typeName) {
	unsigned int count = 0;
	for(Block& block: blocks) {
		if(block.structure->type->name == typeName)
			count++;
	}
	return count;
}

std::vector<File::Block*> File::getBlocksByType(string typeName) {
	std::vector<Block*> ret;
	for(Block& block: blocks) {
		if(block.structure->type->name == typeName)
			ret.push_back(&block);
	}
	return ret;
}

File::Block* File::getBlocksByType(string typeName, unsigned int pos) {
	unsigned int maxNum = getNumberOfTypes(typeName);
	if(maxNum <= pos) {
		ofLogWarning(OFX_BLENDER) << typeName << " " << pos << " not found";
		return NULL;
	}
	return getBlocksByType(typeName)[pos];
}

bool File::doesAddressExist(unsigned long address) {
	for(File::Block& block:blocks) {
		if(block.address == address) {
			return true;
		}
	}
	return false;
}

File::Block* File::getBlockByAddress(unsigned long address) {
	for(File::Block& block:blocks) {
		if(block.address == address) {
			return &block;
		}
	}
	ofLogWarning(OFX_BLENDER) << "Could not find block at address " << address;
	return NULL;
}

////////
unsigned int File::getNumberOfScenes() {
	return getNumberOfTypes(BL_SCENE);
}

Scene* File::getScene(unsigned int index) {
	return static_cast<Scene*>(parseFileBlock(getBlocksByType(BL_SCENE, index)));
}

Scene* File::getScene(string sceneName) {
	std::vector<File::Block*> sceneBlocks = getBlocksByType(BL_SCENE);
	for(File::Block* block:sceneBlocks) {
		Scene* scn = static_cast<Scene*>(parseFileBlock(block));
		if(scn->name == sceneName)
			return scn;
	}
	return NULL;
}

unsigned int File::getNumberOfObjects() {
	return getNumberOfTypes(BL_OBJECT);
}

Object* File::getObject(unsigned int index) {
	return static_cast<Object*>(parseFileBlock(getBlocksByType(BL_OBJECT, index)));
}

Object* File::getObjectByAddress(unsigned long address) {
	return static_cast<Object*>(parseFileBlock(getBlockByAddress(address)));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////// EXPORT HTML //////////
void File::exportStructure(string path) {
	//open file buffer
	std::filebuf fb;
	fb.open(ofToDataPath(path, true).c_str(), std::ios::out);
	std::ostream html(&fb);

	html << "<html><head><title>ofxBlender file structure export</title>";
	html << "<style type=\"text/css\">";
	//styles
	html << "html,body{font-family:monospace}" << endl;
	html << "a{color: #000;}" << endl;
	html << "h2{padding:0;margin: 20px 0 5px 0;}" << endl;
	html << "h3{padding:0;margin: 10px 0 5px 0;}" << endl;
	html << "h3.type{font-weight:normal;}" << endl;
	html << "tr{}" << endl;
	html << "th,td{text-align:left;padding: 7px;border-bottom:1px solid #ccc;margin:0;}" << endl;
	html << "td.center, th.center{text-align:center;}" << endl;
	html << "</style></head><body>";

	html << "<h1>ofxBlender</h1>" << endl;

	html << "<h2>File info</h2>" << endl;
	html << "<h3>version</h3> " << version;
	string pType = "unsigned int";
	if(pointerSize == 8)
		pType = "unsigned long";
	html << "<h3>pointer type</h3> " << pType;


	//write all structures
	html << "<h2>Structures (Size)</h2>" << endl;

	for(vector<DNAStructure>::iterator it = catalog.structures.begin(); it<catalog.structures.end(); it++) {
		html << "<h3 class='type'><a id=\"" << (*it).type->name << "\">";
		html << (*it).type->id << " <b>" << (*it).type->name << "</b> (" << (*it).type->size << ")";
		html << "</a></h3>";
		html << "<table cellspacing='0'><tr><th>TYPE</th><th>NAME CLEAN</th><th>NAME</th><th class='center'>[]</th><th class='center'>*</th><th>SIZE</th><th>OFFSET</th></tr>" << endl;
		for(vector<DNAField>::iterator jt = (*it).fields.begin(); jt<(*it).fields.end(); jt++) {
			html << "<tr>";
			html << "<td>";
			//TYPE
			bool makeLink = catalog.hasStructure((*jt).type->name);
			if(makeLink)
				html << "<a href=\"#" << (*jt).type->name << "\">";
			html << (*jt).type->name;
			if(makeLink)
				html << "</a>";
			html << "</td>";

			//NAME
			html << "<td>" << (*jt).name->nameClean << "</td>";
			html << "<td>" << (*jt).name->name << "</td>";

			//IS ARRAY
			string arrayTxt = "";
			if((*jt).isArray) {
				//arrayTxt = "" + ofToString((*jt).arrayDimensions) + "";
				arrayTxt = "[";
				for(int i: (*jt).arraySizes)
					arrayTxt += ofToString(i) + ",";
				arrayTxt = arrayTxt.substr(0, arrayTxt.size()-1);
				arrayTxt += "]";
			}
			html << "<td class='center'>" << arrayTxt << "</td>";

			//IS POINTER
			string ptrTxt = "";
			if((*jt).isPointer)
				ptrTxt = "&#10003;";
			html << "<td class='center'>" << ptrTxt << "</td>";

			//SIZE
			html << "<td>" << (*jt).type->size << "</td>";
			html << "<td>" << (*jt).offset << "</td>";
			html << "</tr>" << endl;
		}
		html << "</table><br />" << endl;
	}

	//write all data blocks
	html << "<h2>Blocks</h2>" << endl;
	html << "<table cellspacing='0'><tr><th>NAME</th><th>TYPE</th><th>COUNT</th><th>SIZE</th><th>OFFSET</th><th>OLD ADDRESS</th></tr>" << endl;
	for(vector<Block>::iterator it = blocks.begin(); it<blocks.end(); it++) {
		DNAStructure& structure = catalog.structures[(*it).SDNAIndex];
		if(structure.type->name != "ScrVert" && structure.type->name != "Panel" && structure.type->name != "ScrEdge" && structure.type->name != "ARegion") {
			html << "<tr>";
			html << "<td>" << (*it).code << "</td>";
			html << "<td><a href=\"#" << structure.type->name << "\">" << structure.type->name << "</a></td>";
			html << "<td>" << (*it).count << "</td>";
			html << "<td>" << (*it).size << "</td>";
			html << "<td>" << (*it).offset << "</td>";
			html << "<td>" << (*it).address << "</td>";
			html << "</tr>" << endl;
		}
	}
	html << "</table>" << endl;

	//write all types
	/*
	html << "<h2>Types</h2>" << endl;
	html << "<table><tr><th>NAME</th><th>SIZE</th></tr>" << endl;
	for(vector<DNAType>::iterator it = catalog.types.begin(); it<catalog.types.end(); it++){
		html << "<tr>";
		html << "<td>" << (*it).name << "</td>";
		html << "<td>" << (*it).size << "</td>";
		html << "</tr>" << endl;
	}
	html << "</table>" << endl;
	*/

	//write all names
	/*
	html << "<h2>Names</h2>" << endl;
	html << "<p>" << endl;
	for(vector<DNAName>::iterator it = catalog.names.begin(); it<catalog.names.end(); it++){
		html << (*it).name;
		if(it != catalog.names.end() - 1)
			html << ", ";
		html << endl;
	}
	html << "</p>" << endl;
	*/

	//finish html
	html << "</body></html>" << endl;
	fb.close();
}

}
}
