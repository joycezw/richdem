#include <iostream>
#include <iomanip>
#include "richdem/common/version.hpp"
#include "richdem/common/Array2D.hpp"

template<class T>
int PerformAlgorithm(std::string templatefile, std::string inputfile, std::string outputfile, std::string flip, std::string analysis){
  Array2D<T>      raster(inputfile,false);
  Array2D<int8_t> temp  (templatefile,false,0,0,0,0,false,false); //Data type doesn't matter since we're not loading it

  raster.projection   = temp.projection;

  if(flip=="fliph" || flip=="fliphv")
    raster.flipHorz();
  if(flip=="flipv" || flip=="fliphv")
    raster.flipVert();
  if(flip!="fliph" && flip!="flipv" && flip!="fliphv" && flip!="noflip"){
    std::cerr<<"Unrecognised flip directive!"<<std::endl;
    return -1;
  }

  raster.saveGDAL(outputfile,analysis);

  return 0;
}

template< typename... Arguments >
int Router(std::string inputfile, Arguments ... args){
  switch(peekGDALType(inputfile)){
    case GDT_Byte:
      return PerformAlgorithm<uint8_t >(args...);
    case GDT_UInt16:
      return PerformAlgorithm<uint16_t>(args...);
    case GDT_Int16:
      return PerformAlgorithm<int16_t >(args...);
    case GDT_UInt32:
      return PerformAlgorithm<uint32_t>(args...);
    case GDT_Int32:
      return PerformAlgorithm<int32_t >(args...);
    case GDT_Float32:
      return PerformAlgorithm<float   >(args...);
    case GDT_Float64:
      return PerformAlgorithm<double  >(args...);
    case GDT_CInt16:
    case GDT_CInt32:
    case GDT_CFloat32:
    case GDT_CFloat64:
      std::cerr<<"Complex types are unsupported. Sorry!"<<std::endl;
      return -1;
    case GDT_Unknown:
    default:
      std::cerr<<"Unrecognised data type: "<<GDALGetDataTypeName(peekGDALType(inputfile))<<std::endl;
      return -1;
  }
}

int main(int argc, char **argv){
  std::string analysis = PrintRichdemHeader(argc, argv);

  if(argc!=5){
    std::cerr<<argv[0]<<" <Template file> <Input File> <Output File> <fliph/flipv/fliphv/noflip>"<<std::endl;
    return -1;
  }

  Router(argv[2],argv[1],argv[2],argv[3],argv[4],analysis)

  return 0;
}