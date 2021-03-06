//This algorithm is discussed in the manuscript:
//    Barnes, R., 2016. "Parallel priority-flood depression filling for trillion
//    cell digital elevation models on desktops or clusters". Computers &
//    Geosciences. doi:10.1016/j.cageo.2016.07.001
#include "gdal_priv.h"
#include <iostream>
#include <iomanip>
#include <string>
#include <queue>
#include <vector>
#include <limits>
#include <fstream> //For reading layout files
#include <sstream> //Used for parsing the <layout_file>
#include "richdem/common/version.hpp"
#include "richdem/common/Layoutfile.hpp"
#include "richdem/common/communication.hpp"
#include "richdem/common/memory.hpp"
#include "richdem/common/timer.hpp"
#include "richdem/common/Array2D.hpp"
#include "richdem/common/grid_cell.hpp"
#include "Zhou2016pf.hpp"
//#include "Barnes2014pf.hpp" //NOTE: Used only for timing tests

//We use the cstdint library here to ensure that the program behaves as expected
//across platforms, especially with respect to the expected limits of operation
//for data set sizes and labels. For instance, in C++, a signed integer must be
//at least 16 bits, but not necessarily more. We force a minimum of 32 bits as
//this is, after all, for use with large datasets.
#include <cstdint>

//Define operating system appropriate directory separators
#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
  #define SLASH_CHAR "/"
#elif defined(__WIN32__)
  #define SLASH_CHAR "\\"
#endif

//TODO: Is it possible to run this without mpirun if we specify a single node
//job?

//TODO: What are these for?
const int TAG_WHICH_JOB   = 0;
const int TAG_TILE_DATA   = 1;
const int TAG_DONE_FIRST  = 2;
const int TAG_SECOND_DATA = 3;
const int TAG_DONE_SECOND = 4;

const int SYNC_MSG_KILL = 0;
const int JOB_FIRST     = 2;
const int JOB_SECOND    = 3;

const uint8_t FLIP_VERT   = 1;
const uint8_t FLIP_HORZ   = 2;

typedef uint32_t label_t;

class TileInfo{
 private:
  friend class cereal::access;
  template<class Archive>
  void serialize(Archive & ar){
    ar(edge,
       flip,
       x,
       y,
       width,
       height,
       gridx,
       gridy,
       nullTile,
       filename,
       outputname,
       retention,
       many,
       analysis);
  }
 public:
  uint8_t     edge;
  uint8_t     flip;
  int32_t     x,y,gridx,gridy,width,height;
  bool        nullTile;
  bool        many;
  label_t     label_offset,label_increment; //Used for convenience in Producer()
  std::string filename;
  std::string outputname;
  std::string retention;
  std::string analysis;   //Command line command used to invoke everything
  TileInfo(){
    nullTile = true;
  }
  TileInfo(std::string filename, std::string outputname, std::string retention, int32_t gridx, int32_t gridy, int32_t x, int32_t y, int32_t width, int32_t height, bool many, std::string analysis){
    this->nullTile   = false;
    this->edge       = 0;
    this->x          = x;
    this->y          = y;
    this->width      = width;
    this->height     = height;   
    this->gridx      = gridx;
    this->gridy      = gridy;
    this->filename   = filename;
    this->outputname = outputname;
    this->retention  = retention;
    this->flip       = 0;
    this->many       = many;
    this->analysis   = analysis;
  }
};

typedef std::vector< std::vector< TileInfo > > TileGrid;


class TimeInfo {
 private:
  friend class cereal::access;
  template<class Archive>
  void serialize(Archive & ar){
    ar(calc,overall,io,vmpeak,vmhwm);
  }
 public:
  double calc, overall, io;
  long vmpeak, vmhwm;
  TimeInfo() {
    calc=overall=io=0;
    vmpeak=vmhwm=0;
  }
  TimeInfo(double calc, double overall, double io, long vmpeak, long vmhwm) :
      calc(calc), overall(overall), io(io), vmpeak(vmpeak), vmhwm(vmhwm) {}
  TimeInfo& operator+=(const TimeInfo& o){
    calc    += o.calc;
    overall += o.overall;
    io      += o.io;
    vmpeak   = std::max(vmpeak,o.vmpeak);
    vmhwm    = std::max(vmhwm,o.vmhwm);
    return *this;
  }
};

template<class elev_t>
class Job1 {
 private:
  friend class cereal::access;
  template<class Archive>
  void serialize(Archive & ar){
    ar(top_elev,
       bot_elev,
       left_elev,
       right_elev,
       top_label,
       bot_label,
       left_label,
       right_label,
       graph,
       time_info,
       gridy,
       gridx);
  }
 public:
  std::vector<elev_t > top_elev,  bot_elev,  left_elev,  right_elev;  //TODO: Consider using std::array instead
  std::vector<label_t> top_label, bot_label, left_label, right_label; //TODO: Consider using std::array instead
  std::vector< std::map<label_t, elev_t> > graph;
  TimeInfo time_info;
  int gridy, gridx;
  Job1(){}
};



template<class elev_t> using Job1Grid    = std::vector< std::vector< Job1<elev_t> > >;
template<class elev_t> using Job2        = std::vector<elev_t>;
template<class elev_t> using StorageType = std::map<std::pair<int,int>, std::pair< Array2D<elev_t>, Array2D<label_t> > >;



int SuggestTileSize(int selected, int size, int min){
  int best=999999999;
  for(int x=1;x<size;x++)
    if(size%x>min && std::abs(x-selected)<std::abs(x-best))
      best=x;
  return best;
}


template<class elev_t>
class ConsumerSpecifics {
 public:
  Array2D<elev_t>     dem;
  Array2D<label_t>    labels;
  std::vector< std::map<label_t, elev_t> > spillover_graph;
  Timer timer_io;
  Timer timer_calc;

  void LoadFromEvict(const TileInfo &tile){
    //The upper limit on unique watersheds is the number of edge cells. Resize
    //the graph to this number. The Priority-Flood routines will shrink it to
    //the actual number needed.
    spillover_graph.resize(2*tile.width+2*tile.height);

    //Read in the data associated with the job
    timer_io.start();
    dem = Array2D<elev_t>(tile.filename, false, tile.x, tile.y, tile.width, tile.height, tile.many);
    timer_io.stop();

    //TODO: Figure out a clever way to allow tiles of different widths/heights
    if(dem.width()!=tile.width){
      std::cerr<<"Tile '"<<tile.filename<<"' had unexpected width. Found "<<dem.width()<<" expected "<<tile.width<<std::endl;
      throw std::runtime_error("Unexpected width.");
    }

    if(dem.height()!=tile.height){
      std::cerr<<"Tile '"<<tile.filename<<"' had unexpected height. Found "<<dem.height()<<" expected "<<tile.height<<std::endl;
      throw std::runtime_error("Unexpected height.");
    }

    //These variables are needed by Priority-Flood. The internal
    //interconnections of labeled regions (named "graph") are also needed to
    //solve the problem, but that can be passed directly from the job object.
    labels = Array2D<label_t>(dem,0);

    //Perform the watershed Priority-Flood algorithm on the tile. The variant
    //by Zhou, Sun, and Fu (2015) is used for this; however, I have modified
    //their algorithm to label watersheds similarly to what is described in
    //Barnes, Lehman, and Mulla (2014). Note that the Priority-Flood needs to
    //know whether the tile is being flipped since it uses this information to
    //determine which edges connect to Special Watershed 1 (which is the
    //outside of the DEM as a whole).
    timer_calc.start();
    Zhou2015Labels(dem, labels, spillover_graph, tile.edge, tile.flip & FLIP_HORZ, tile.flip & FLIP_VERT);
    timer_calc.stop();
  }

  void VerifyInputSanity(){
    //Nothing to verify
  }

  void SaveToCache(const TileInfo &tile){
    timer_io.start();
    dem.setCacheFilename(tile.retention+"dem.dat");
    labels.setCacheFilename(tile.retention+"dem.dat");
    dem.dumpData();
    labels.dumpData();
    timer_io.stop();
  }

  void LoadFromCache(const TileInfo &tile){
    timer_io.start();
    dem    = Array2D<elev_t >(tile.retention+"dem.dat"   ,true); //TODO: There should be an exception if this fails
    labels = Array2D<label_t>(tile.retention+"labels.dat",true);
    timer_io.stop();
  }

  void SaveToRetain(TileInfo &tile, StorageType<elev_t> &storage){
    timer_io.start();
    auto &temp  = storage[std::make_pair(tile.gridy,tile.gridx)];
    temp.first  = std::move(dem);
    temp.second = std::move(labels);
    timer_io.stop();
  }

  void LoadFromRetain(TileInfo &tile, StorageType<elev_t> &storage){
    timer_io.start();
    auto &temp = storage.at(std::make_pair(tile.gridy,tile.gridx));
    dem        = std::move(temp.first);
    labels     = std::move(temp.second);
    timer_io.stop();
  }

  void FirstRound(const TileInfo &tile, Job1<elev_t> &job1){
    job1.graph = std::move(spillover_graph);

    //The tile's edge info is needed to solve the global problem. Collect it.
    job1.top_elev    = dem.topRow     ();
    job1.bot_elev    = dem.bottomRow  ();
    job1.left_elev   = dem.leftColumn ();
    job1.right_elev  = dem.rightColumn();

    job1.top_label   = labels.topRow     ();
    job1.bot_label   = labels.bottomRow  ();
    job1.left_label  = labels.leftColumn ();
    job1.right_label = labels.rightColumn();


    //Flip the tile if necessary. We could flip the entire tile, but this
    //requires expensive memory shuffling. Instead, we flip just the perimeter
    //of the tile and send the flipped perimeter to the Producer. Notice,
    //though, that tiles adjacent to the edge of the DEM need to be treated
    //specially, which is why the Priority-Flood performed on each tile
    //(above) needs to have knowledge of whether the tile is being flipped.
    if(tile.flip & FLIP_VERT){
      job1.top_elev.swap(job1.bot_elev);
      job1.top_label.swap(job1.bot_label);
      std::reverse(job1.left_elev.begin(),job1.left_elev.end());
      std::reverse(job1.right_elev.begin(),job1.right_elev.end());
      std::reverse(job1.left_label.begin(), job1.left_label.end());
      std::reverse(job1.right_label.begin(),job1.right_label.end());
    }
    if(tile.flip & FLIP_HORZ){
      job1.left_elev.swap(job1.right_elev);
      job1.left_label.swap(job1.right_label);
      std::reverse(job1.top_elev.begin(),job1.top_elev.end());
      std::reverse(job1.bot_elev.begin(),job1.bot_elev.end());
      std::reverse(job1.top_label.begin(),job1.top_label.end());
      std::reverse(job1.bot_label.begin(),job1.bot_label.end());
    }
  }

  void SecondRound(const TileInfo &tile, Job2<elev_t> &job2){
    timer_calc.start();
    for(int32_t y=0;y<dem.height();y++)
    for(int32_t x=0;x<dem.width();x++)
      if(labels(x,y)>1 && dem(x,y)<job2.at(labels(x,y)))
        dem(x,y) = job2.at(labels(x,y));
    timer_calc.stop();

    //At this point we're done with the calculation! Boo-yeah!

    dem.printStamp(5,"Unorientated output stamp");

    timer_io.start();
    dem.saveGDAL(tile.outputname, tile.analysis, tile.x, tile.y);
    timer_io.stop();
  }
};




template<class elev_t>
class ProducerSpecifics {
 public:
  Timer timer_io, timer_calc;

 private:
  std::vector<elev_t> graph_elev;

  void HandleEdge(
    const std::vector<elev_t>  &elev_a,
    const std::vector<elev_t>  &elev_b,
    const std::vector<label_t> &label_a,
    const std::vector<label_t> &label_b,
    std::vector< std::map<label_t, elev_t> > &mastergraph,
    const label_t label_a_offset,
    const label_t label_b_offset
  ){
    //Guarantee that all vectors are of the same length
    assert(elev_a.size ()==elev_b.size ());
    assert(label_a.size()==label_b.size());
    assert(elev_a.size ()==label_b.size());

    int len = elev_a.size();

    for(int i=0;i<len;i++){
      auto c_l = label_a[i];
      if(c_l>1) c_l+=label_a_offset;

      for(int ni=i-1;ni<=i+1;ni++){
        if(ni<0 || ni==len)
          continue;
        auto n_l = label_b[ni];
        if(n_l>1) n_l+=label_b_offset;
        //TODO: Does this really matter? We could just ignore these entries
        if(c_l==n_l) //Only happens when labels are both 1
          continue;

        auto elev_over = std::max(elev_a[i],elev_b[ni]);
        if(mastergraph.at(c_l).count(n_l)==0 || elev_over<mastergraph.at(c_l)[n_l]){
          mastergraph[c_l][n_l] = elev_over;
          mastergraph[n_l][c_l] = elev_over;
        }
      }
    }
  }

  void HandleCorner(
    const elev_t  elev_a,
    const elev_t  elev_b,
    label_t       l_a,
    label_t       l_b,
    std::vector< std::map<label_t, elev_t> > &mastergraph,
    const label_t l_a_offset,
    const label_t l_b_offset
  ){
    if(l_a>1) l_a += l_a_offset;
    if(l_b>1) l_b += l_b_offset;
    auto elev_over = std::max(elev_a,elev_b);
    if(mastergraph.at(l_a).count(l_b)==0 || elev_over<mastergraph.at(l_a)[l_b]){
      mastergraph[l_a][l_b] = elev_over;
      mastergraph[l_b][l_a] = elev_over;
    }
  }

 public:
  void Calculations(TileGrid &tiles, Job1Grid<elev_t> &jobs1){
    //Merge all of the graphs together into one very big graph. Clear information
    //as we go in order to save space, though I am not sure if the map::clear()
    //method is not guaranteed to release space.
    std::cerr<<"Constructing mastergraph..."<<std::endl;
    std::cerr<<"Merging graphs..."<<std::endl;
    timer_calc.start();
    Timer timer_mg_construct;
    timer_mg_construct.start();

    const int gridheight = tiles.size();
    const int gridwidth  = tiles[0].size();
    
    //Get a tile size so we can calculate the max label
    label_t maxlabel = 0;
    for(int y=0;y<gridheight;y++)
    for(int x=0;x<gridwidth;x++)
      maxlabel+=jobs1[y][x].graph.size();
    std::cerr<<"!Total labels required: "<<maxlabel<<std::endl;

    std::vector< std::map<label_t, elev_t> > mastergraph(maxlabel);

    label_t label_offset = 0;
    for(int y=0;y<gridheight;y++)
    for(int x=0;x<gridwidth;x++){
      if(tiles[y][x].nullTile)
        continue;

      auto &this_job = jobs1.at(y).at(x);

      tiles[y][x].label_offset = label_offset;

      for(int l=0;l<(int)this_job.graph.size();l++)
      for(auto const &skey: this_job.graph[l]){
        label_t first_label  = l;
        label_t second_label = skey.first;
        if(first_label >1) first_label +=label_offset;
        if(second_label>1) second_label+=label_offset;
        //We insert both ends of the bidirectional edge because in the watershed
        //labeling process, we only inserted one. We need both here because we
        //don't know which end of the edge we will approach from as we traverse
        //the spillover graph.
        mastergraph.at(first_label)[second_label] = skey.second;
        mastergraph.at(second_label)[first_label] = skey.second;
      }
      tiles[y][x].label_increment = this_job.graph.size();
      label_offset                += this_job.graph.size();
      this_job.graph.clear();
    }

    std::cerr<<"Handling adjacent edges and corners..."<<std::endl;
    for(int y=0;y<gridheight;y++)
    for(int x=0;x<gridwidth;x++){
      if(tiles[y][x].nullTile)
        continue;

      auto &c = jobs1[y][x];

      if(y>0            && !tiles[y-1][x].nullTile)
        HandleEdge(c.top_elev,   jobs1[y-1][x].bot_elev,   c.top_label,   jobs1[y-1][x].bot_label,   mastergraph, tiles[y][x].label_offset, tiles[y-1][x].label_offset);

      if(y<gridheight-1 && !tiles[y+1][x].nullTile)
        HandleEdge(c.bot_elev,   jobs1[y+1][x].top_elev,   c.bot_label,   jobs1[y+1][x].top_label,   mastergraph, tiles[y][x].label_offset, tiles[y+1][x].label_offset);
      
      if(x>0            && !tiles[y][x-1].nullTile)
        HandleEdge(c.left_elev,  jobs1[y][x-1].right_elev, c.left_label,  jobs1[y][x-1].right_label, mastergraph, tiles[y][x].label_offset, tiles[y][x-1].label_offset);
      
      if(x<gridwidth-1  && !tiles[y][x+1].nullTile)
        HandleEdge(c.right_elev, jobs1[y][x+1].left_elev,  c.right_label, jobs1[y][x+1].left_label,  mastergraph, tiles[y][x].label_offset, tiles[y][x+1].label_offset);


      //I wish I had wrote it all in LISP.
      //Top left
      if(y>0 && x>0                      && !tiles[y-1][x-1].nullTile)   
        HandleCorner(c.top_elev.front(), jobs1[y-1][x-1].bot_elev.back(),  c.top_label.front(), jobs1[y-1][x-1].bot_label.back(),  mastergraph, tiles[y][x].label_offset, tiles[y-1][x-1].label_offset);
      
      //Bottom right
      if(y<gridheight-1 && x<gridwidth-1 && !tiles[y+1][x+1].nullTile) 
        HandleCorner(c.bot_elev.back(),  jobs1[y+1][x+1].top_elev.front(), c.bot_label.back(),  jobs1[y+1][x+1].top_label.front(), mastergraph, tiles[y][x].label_offset, tiles[y+1][x+1].label_offset);
      
      //Top right
      if(y>0 && x<gridwidth-1            && !tiles[y-1][x+1].nullTile)            
        HandleCorner(c.top_elev.back(),  jobs1[y-1][x+1].bot_elev.front(), c.top_label.back(),  jobs1[y-1][x+1].bot_label.front(), mastergraph, tiles[y][x].label_offset, tiles[y-1][x+1].label_offset);
      
      //Bottom left
      if(x>0 && y<gridheight-1           && !tiles[y+1][x-1].nullTile) 
        HandleCorner(c.bot_elev.front(), jobs1[y+1][x-1].top_elev.back(),  c.bot_label.front(), jobs1[y+1][x-1].top_label.back(),  mastergraph, tiles[y][x].label_offset, tiles[y+1][x-1].label_offset);
    }
    timer_mg_construct.stop();

    std::cerr<<"!Mastergraph constructed in "<<timer_mg_construct.accumulated()<<"s. "<<std::endl;

    //Clear the jobs1 data from memory since we no longer need it
    jobs1.clear();
    jobs1.shrink_to_fit();


    std::cerr<<"Performing aggregated priority flood"<<std::endl;
    Timer agg_pflood_timer;
    agg_pflood_timer.start();
    typedef std::pair<elev_t, label_t>  graph_node;
    std::priority_queue<graph_node, std::vector<graph_node>, std::greater<graph_node> > open;
    std::queue<graph_node> pit;
    std::vector<bool>   visited(maxlabel,false); //TODO
    graph_elev.resize(maxlabel);                 //TODO

    open.emplace(std::numeric_limits<elev_t>::lowest(),1);

    while(open.size()>0 || pit.size()>0){
      graph_node c;
      if(pit.size()>0){
        c = pit.front();
        pit.pop();
      } else {
        c = open.top();
        open.pop();
      }

      auto my_elev       = c.first;
      auto my_vertex_num = c.second;
      if(visited[my_vertex_num])
        continue;

      graph_elev[my_vertex_num] = my_elev;
      visited   [my_vertex_num] = true;

      for(auto &n: mastergraph[my_vertex_num]){
        auto n_vertex_num = n.first;
        auto n_elev       = n.second;
        if(visited[n_vertex_num])
          continue;
        open.emplace(std::max(my_elev,n_elev),n_vertex_num);
        //Turning on these lines activates the improved priority flood. It is
        //disabled to make the algorithm easier to verify by inspection, and
        //because it made little difference in the overall speed of the algorithm.

        // if(n_elev<=my_elev){
        //   pit.emplace(my_elev,n_vertex_num);
        // } else {
        //   open.emplace(n_elev,n_vertex_num);
        // }
      }
    }
    agg_pflood_timer.stop();
    std::cerr<<"!Aggregated priority flood time: "<<agg_pflood_timer.accumulated()<<"s."<<std::endl;
    timer_calc.stop();
  }

  Job2<elev_t> DistributeJob2(const TileGrid &tiles, int tx, int ty){
    timer_calc.start();
    auto job2 = Job2<elev_t>(graph_elev.begin()+tiles[ty][tx].label_offset,graph_elev.begin()+tiles[ty][tx].label_offset+tiles[ty][tx].label_increment);
    timer_calc.stop();
    return job2;
  }
};















































template<class T>
void Consumer(){
  TileInfo      tile;
  StorageType<T> storage;

  //Have the consumer process messages as long as they are coming using a
  //blocking receive to wait.
  while(true){
    // When probe returns, the status object has the size and other attributes
    // of the incoming message. Get the message size. TODO
    int the_job = CommGetTag(0);

    //This message indicates that everything is done and the Consumer should shut
    //down.
    if(the_job==SYNC_MSG_KILL){
      return;

    //This message indicates that the consumer should prepare to perform the
    //first part of the distributed Priority-Flood algorithm on an incoming job
    } else if (the_job==JOB_FIRST){
      Timer timer_overall;
      timer_overall.start();
      
      CommRecv(&tile, nullptr, 0);

      ConsumerSpecifics<T> consumer;
      Job1<T>              job1;

      job1.gridy = tile.gridy;
      job1.gridx = tile.gridx;

      consumer.LoadFromEvict(tile);
      consumer.VerifyInputSanity();

      consumer.FirstRound(tile, job1);

      if(tile.retention=="@evict"){
        //Nothing to do: it will all get overwritten
      } else if(tile.retention=="@retain"){
        consumer.SaveToRetain(tile,storage);
      } else {
        consumer.SaveToCache(tile);
      }

      timer_overall.stop();

      long vmpeak, vmhwm;
      ProcessMemUsage(vmpeak,vmhwm);

      job1.time_info = TimeInfo(consumer.timer_calc.accumulated(),timer_overall.accumulated(),consumer.timer_io.accumulated(),vmpeak,vmhwm);

      CommSend(&job1,nullptr,0,TAG_DONE_FIRST);
    } else if (the_job==JOB_SECOND){
      Timer timer_overall;
      timer_overall.start();

      ConsumerSpecifics<T> consumer;
      Job2<T>              job2;

      CommRecv(&tile, &job2, 0);

      //These use the same logic as the analogous lines above
      if(tile.retention=="@evict")
        consumer.LoadFromEvict(tile);
      else if(tile.retention=="@retain")
        consumer.LoadFromRetain(tile,storage);
      else
        consumer.LoadFromCache(tile);

      consumer.SecondRound(tile, job2);

      timer_overall.stop();

      long vmpeak, vmhwm;
      ProcessMemUsage(vmpeak,vmhwm);

      TimeInfo temp(consumer.timer_calc.accumulated(), timer_overall.accumulated(), consumer.timer_io.accumulated(),vmpeak,vmhwm);
      CommSend(&temp, nullptr, 0, TAG_DONE_SECOND);
    }
  }
}







//Producer takes a collection of Jobs and delegates them to Consumers. Once all
//of the jobs have received their initial processing, it uses that information
//to compute the global properties necessary to the solution. Each Job, suitably
//modified, is then redelegated to a Consumer which ultimately finishes the
//processing.
template<class T>
void Producer(TileGrid &tiles){
  Timer timer_overall;
  timer_overall.start();

  ProducerSpecifics<T> producer;

  const int gridheight = tiles.size();
  const int gridwidth  = tiles.front().size();

  //How many processes to send to
  const int active_consumer_limit = CommSize()-1;
  //Used to hold message buffers while non-blocking sends are used
  std::vector<msg_type> msgs;
  //Number of jobs for which we are waiting for a return
  int jobs_out=0;

  ////////////////////////////////////////////////////////////
  //SEND JOBS

  //Distribute jobs to the consumers. Since this is non-blocking, all of the
  //jobs will be sent and then we will wait to hear back below.
  for(int y=0;y<gridheight;y++)
  for(int x=0;x<gridwidth;x++){
    if(tiles[y][x].nullTile)
      continue;

    msgs.push_back(CommPrepare(&tiles.at(y).at(x),nullptr));
    CommISend(msgs.back(), (jobs_out%active_consumer_limit)+1, JOB_FIRST);
    jobs_out++;
  }

  //NOTE: As each job returns partial reductions could be done on the data to
  //reduce the amount of memory required by the master node. That is, the full
  //reduction below, which is done on all of the collected data at once, could
  //be broken up. The downside to this would be a potentially significant
  //increase in the complexity of this code. I have opted for a more resource-
  //intensive implementation in order to try to keep the code simple.

  std::cerr<<"m Jobs created = "<<jobs_out<<std::endl;

  //Grid to hold returned jobs
  Job1Grid<T> jobs1(tiles.size(), std::vector< Job1<T> >(tiles[0].size()));
  while(jobs_out--){
    std::cerr<<"p Jobs remaining = "<<jobs_out<<std::endl;
    Job1<T> temp;
    CommRecv(&temp, nullptr, -1);
    jobs1.at(temp.gridy).at(temp.gridx) = temp;
  }

  std::cerr<<"n First stage Tx = "<<CommBytesSent()<<" B"<<std::endl;
  std::cerr<<"n First stage Rx = "<<CommBytesRecv()<<" B"<<std::endl;
  CommBytesReset();

  //Get timing info
  TimeInfo time_first_total;
  for(int y=0;y<gridheight;y++)
  for(int x=0;x<gridwidth;x++)
    time_first_total += jobs1[y][x].time_info;


  ////////////////////////////////////////////////////////////
  //PRODUCER NODE PERFORMS PROCESSING ON ALL THE RETURNED DATA

  producer.Calculations(tiles,jobs1);

  ////////////////////////////////////////////////////////////
  //SEND OUT JOBS TO FINALIZE GLOBAL SOLUTION

  //Reset these two variables
  jobs_out = 0; 
  msgs     = std::vector<msg_type>();

  for(int y=0;y<gridheight;y++)
  for(int x=0;x<gridwidth;x++){
    if(tiles[y][x].nullTile)
      continue;

    auto job2 = producer.DistributeJob2(tiles, x, y);

    msgs.push_back(CommPrepare(&tiles.at(y).at(x),&job2));
    CommISend(msgs.back(), (jobs_out%active_consumer_limit)+1, JOB_SECOND);
    jobs_out++;
  }

  //There's no further processing to be done at this point, but we'll gather
  //timing and memory statistics from the consumers.
  TimeInfo time_second_total;

  while(jobs_out--){
    std::cerr<<"p Jobs left to receive = "<<jobs_out<<std::endl;
    TimeInfo temp;
    CommRecv(&temp, nullptr, -1);
    time_second_total += temp;
  }

  //Send out a message to tell the consumers to politely quit. Their job is
  //done.
  for(int i=1;i<CommSize();i++){
    int temp;
    CommSend(&temp,nullptr,i,SYNC_MSG_KILL);
  }

  timer_overall.stop();

  std::cerr<<"t First stage total overall time = "<<time_first_total.overall<<" s"<<std::endl;
  std::cerr<<"t First stage total io time = "     <<time_first_total.io     <<" s"<<std::endl;
  std::cerr<<"t First stage total calc time = "   <<time_first_total.calc   <<" s"<<std::endl;
  std::cerr<<"r First stage peak child VmPeak = " <<time_first_total.vmpeak <<std::endl;
  std::cerr<<"r First stage peak child VmHWM = "  <<time_first_total.vmhwm  <<std::endl;

  std::cerr<<"n Second stage Tx = "<<CommBytesSent()<<" B"<<std::endl;
  std::cerr<<"n Second stage Rx = "<<CommBytesRecv()<<" B"<<std::endl;

  std::cerr<<"t Second stage total overall time = "<<time_second_total.overall<<" s"<<std::endl;
  std::cerr<<"t Second stage total IO time = "     <<time_second_total.io     <<" s"<<std::endl;
  std::cerr<<"t Second stage total calc time = "   <<time_second_total.calc   <<" s"<<std::endl;
  std::cerr<<"r Second stage peak child VmPeak = " <<time_second_total.vmpeak <<std::endl;
  std::cerr<<"r Second stage peak child VmHWM = "  <<time_second_total.vmhwm  <<std::endl;

  std::cerr<<"t Producer overall time = "<<timer_overall.accumulated()       <<" s"<<std::endl;
  std::cerr<<"t Producer calc time = "   <<producer.timer_calc.accumulated() <<" s"<<std::endl;

  long vmpeak, vmhwm;
  ProcessMemUsage(vmpeak,vmhwm);
  std::cerr<<"r Producer's VmPeak = "   <<vmpeak <<std::endl;
  std::cerr<<"r Producer's VmHWM = "    <<vmhwm  <<std::endl;
}



//Preparer divides up the input raster file into tiles which can be processed
//independently by the Consumers. Since the tileing may be done on-the-fly or
//rely on preparation the user has done, the Preparer routine knows how to deal
//with both. Once assemebled, the collection of jobs is passed off to Producer,
//which is agnostic as to the original form of the jobs and handles
//communication and solution assembly.
void Preparer(
  std::string many_or_one,
  const std::string retention,
  const std::string input_file,
  const std::string output_name,
  int bwidth,
  int bheight,
  int flipH,
  int flipV,
  std::string analysis
){
  Timer timer_overall;
  timer_overall.start();

  TileGrid tiles;
  std::string  filename;
  GDALDataType file_type;        //All tiles must have a common file_type
  TileInfo *reptile = nullptr; //Pointer to a representative tile

  std::string output_layout_name = output_name;
  if(output_name.find("%f")!=std::string::npos){
    output_layout_name.replace(output_layout_name.find("%f"), 2, "layout");
  } else if(output_name.find("%n")!=std::string::npos){
    output_layout_name.replace(output_layout_name.find("%n"), 2, "layout");
  } else { //Should never happen
    std::cerr<<"E Outputname for mode-many must contain '%f' or '%n'!"<<std::endl;
    throw std::runtime_error("Outputname for mode-many must contain '%f' or '%n'!");
  }
  LayoutfileWriter lfout(output_layout_name);

  if(many_or_one=="many"){
    int32_t tile_width     = -1; //Width of 1st tile. All tiles must equal this
    int32_t tile_height    = -1; //Height of 1st tile, all tiles must equal this
    long    cell_count     = 0;
    int     not_null_tiles = 0;
    std::vector<double> tile_geotransform(6);

    LayoutfileReader lf(input_file);

    while(lf.next()){
      if(lf.newRow()){ //Add a row to the grid of tiles
        tiles.emplace_back();
        lfout.addRow();
      }

      if(lf.isNullTile()){
        tiles.back().emplace_back();
        lfout.addEntry(""); //Add a null tile to the output
        continue;
      }

      not_null_tiles++;

      if(tile_height==-1){
        //Retrieve information about this tile. All tiles must have the same
        //dimensions, which we could check here, but opening and closing
        //thousands of files is expensive. Therefore, we rely on the user to
        //check this beforehand if they want to. We will, however, verify that
        //things are correct in Consumer() as we open the files for reading.
        try{
          getGDALDimensions(lf.getFullPath(),tile_height,tile_width,file_type,tile_geotransform.data());
        } catch (...) {
          std::cerr<<"E Error getting file information from '"<<lf.getFullPath()<<"'!"<<std::endl;
          CommAbort(-1); //TODO
        }
      }

      cell_count += tile_width*tile_height;

      std::string this_retention = retention;
      if(retention.find("%f")!=std::string::npos){
        this_retention.replace(this_retention.find("%f"), 2, lf.getBasename());          
      } else if(retention.find("%n")!=std::string::npos){
        this_retention.replace(this_retention.find("%n"), 2, lf.getGridLocName());
      } else if(retention[0]=='@') {
        this_retention = retention;
      } else { //Should never happen
        std::cerr<<"E Outputname for mode-many must contain '%f' or '%n'!"<<std::endl;
        throw std::runtime_error("Outputname for mode-many must contain '%f' or '%n'!");
      }

      std::string this_output_name = output_name;
      if(output_name.find("%f")!=std::string::npos){
        this_output_name.replace(this_output_name.find("%f"), 2, lf.getBasename());          
      } else if(output_name.find("%n")!=std::string::npos){
        this_output_name.replace(this_output_name.find("%n"), 2, lf.getGridLocName());
      } else { //Should never happen
        std::cerr<<"E Outputname for mode-many must contain '%f' or '%n'!"<<std::endl;
        throw std::runtime_error("Outputname for mode-many must contain '%f' or '%n'!");
      }

      tiles.back().emplace_back(
        lf.getFullPath(),
        this_output_name,
        this_retention,
        lf.getX(),
        lf.getY(),
        0,
        0,
        tile_width,
        tile_height,
        true,
        analysis
      );

      //Get a representative tile, if we don't already have one
      if(reptile==nullptr)
        reptile = &tiles.back().back();

      lfout.addEntry(this_output_name);

      //Flip tiles if the geotransform demands it
      if(tile_geotransform[1]<0)
        tiles.back().back().flip ^= FLIP_HORZ;
      if(tile_geotransform[5]>0)
        tiles.back().back().flip ^= FLIP_VERT;

      //Flip (or reverse the above flip!) if the user demands it
      if(flipH)
        tiles.back().back().flip ^= FLIP_HORZ;
      if(flipV)
        tiles.back().back().flip ^= FLIP_VERT;
    }

    std::cerr<<"c Loaded "<<tiles.size()<<" rows each of which had "<<tiles[0].size()<<" columns."<<std::endl;
    std::cerr<<"m Total cells to be processed = "<<cell_count<<std::endl;
    std::cerr<<"m Number of tiles which were not null = "<<not_null_tiles<<std::endl;

    //nullTiles imply that the tiles around them have edges, as though they
    //are on the edge of the raster.
    for(int y=0;y<(int)tiles.size();y++)
    for(int x=0;x<(int)tiles[0].size();x++){
      if(tiles[y][x].nullTile)
        continue;
      if(y-1>0 && x>0 && tiles[y-1][x].nullTile)
        tiles[y][x].edge |= GRID_TOP;
      if(y+1<(int)tiles.size() && x>0 && tiles[y+1][x].nullTile)
        tiles[y][x].edge |= GRID_BOTTOM;
      if(y>0 && x-1>0 && tiles[y][x-1].nullTile)
        tiles[y][x].edge |= GRID_LEFT;
      if(y>0 && x+1<(int)tiles[0].size() && tiles[y][x+1].nullTile)
        tiles[y][x].edge |= GRID_RIGHT;
    }

  } else if(many_or_one=="one") {
    int32_t total_height;
    int32_t total_width;

    //Get the total dimensions of the input file
    try {
      getGDALDimensions(input_file, total_height, total_width, file_type, NULL);
    } catch (...) {
      std::cerr<<"E Error getting file information from '"<<input_file<<"'!"<<std::endl;
      CommAbort(-1); //TODO
    }

    //If the user has specified -1, that implies that they want the entire
    //dimension of the raster along the indicated axis to be processed within a
    //single job.
    if(bwidth==-1)
      bwidth  = total_width;
    if(bheight==-1)
      bheight = total_height;

    std::cerr<<"m Total width =  "<<total_width <<"\n";
    std::cerr<<"m Total height = "<<total_height<<"\n";
    std::cerr<<"m Block width =  "<<bwidth      <<"\n";
    std::cerr<<"m Block height = "<<bheight     <<std::endl;
    std::cerr<<"m Total cells to be processed = "<<(total_width*total_height)<<std::endl;

    //Create a grid of jobs
    //TODO: Avoid creating extremely narrow or small strips
    for(int32_t y=0,gridy=0;y<total_height; y+=bheight, gridy++){
      tiles.emplace_back(std::vector<TileInfo>());
      for(int32_t x=0,gridx=0;x<total_width;x+=bwidth,  gridx++){
        if(total_height-y<100){
          std::cerr<<"At least one tile is <100 cells in height. Please change rectangle size to avoid this!"<<std::endl;
          std::cerr<<"I suggest you use bheight="<<SuggestTileSize(bheight, total_height, 100)<<std::endl;
          throw std::logic_error("Tile height too small!");
        }
        if(total_width -x<100){
          std::cerr<<"At least one tile is <100 cells in width. Please change rectangle size to avoid this!"<<std::endl;
          std::cerr<<"I suggest you use bwidth="<<SuggestTileSize(bwidth, total_width, 100)<<std::endl;
          throw std::logic_error("Tile width too small!");
        }

        if(retention[0]!='@' && retention.find("%n")==std::string::npos){
          std::cerr<<"E In <one> mode '%n' must be present in the retention path."<<std::endl;
          throw std::invalid_argument("'%n' not found in retention path!");
        }

        if(output_name.find("%n")==std::string::npos){
          std::cerr<<"E In <one> mode '%n' must be present in the output path."<<std::endl;
          throw std::invalid_argument("'%n' not found in output path!");
        }

        //Used for '%n' formatting
        std::string coord_string = std::to_string(gridx)+"_"+std::to_string(gridy);

        std::string this_retention = retention;
        if(this_retention[0]!='@')
          this_retention.replace(this_retention.find("%n"), 2, coord_string);
        std::string this_output_name = output_name;
        if(this_output_name.find("%n")==std::string::npos){
          std::cerr<<"E Outputname must include '%n' for <one> mode."<<std::endl;
          throw std::runtime_error("Outputname must include '%n' for <one> mode.");
        }
        this_output_name.replace(this_output_name.find("%n"),2,coord_string);

        tiles.back().emplace_back(
          input_file,
          this_output_name,
          this_retention,
          gridx,
          gridy,
          x,
          y,
          (total_width-x >=bwidth )?bwidth :total_width -x, //TODO: Check
          (total_height-y>=bheight)?bheight:total_height-y,
          false,
          analysis
        );
      }
    }

  } else {
    std::cout<<"Unrecognised option! Must be 'many' or 'one'!"<<std::endl;
    CommAbort(-1);
  }

  //If a job is on the edge of the raster, mark it as having this property so
  //that it can be handled with elegance later.
  for(auto &e: tiles.front())
    e.edge |= GRID_TOP;
  for(auto &e: tiles.back())
    e.edge |= GRID_BOTTOM;
  for(size_t y=0;y<tiles.size();y++){
    tiles[y].front().edge |= GRID_LEFT;
    tiles[y].back().edge  |= GRID_RIGHT;
  }

  CommBroadcast(&file_type,0);
  timer_overall.stop();
  std::cerr<<"t Preparer time = "<<timer_overall.accumulated()<<" s"<<std::endl;

  std::cerr<<"c Flip horizontal = "<<((reptile->flip & FLIP_HORZ)?"YES":"NO")<<std::endl;
  std::cerr<<"c Flip vertical =   "<<((reptile->flip & FLIP_VERT)?"YES":"NO")<<std::endl;
  std::cerr<<"c Input data type = "<<GDALGetDataTypeName(file_type)<<std::endl;

  switch(file_type){
    case GDT_Byte:
      return Producer<uint8_t >(tiles);
    case GDT_UInt16:
      return Producer<uint16_t>(tiles);
    case GDT_Int16:
      return Producer<int16_t >(tiles);
    case GDT_UInt32:
      return Producer<uint32_t>(tiles);
    case GDT_Int32:
      return Producer<int32_t >(tiles);
    case GDT_Float32:
      return Producer<float   >(tiles);
    case GDT_Float64:
      return Producer<double  >(tiles);
    case GDT_CInt16:
    case GDT_CInt32:
    case GDT_CFloat32:
    case GDT_CFloat64:
      std::cerr<<"E Complex types are not supported. Sorry!"<<std::endl;
      CommAbort(-1); //TODO
    case GDT_Unknown:
    default:
      std::cerr<<"E Unrecognised data type: "<<GDALGetDataTypeName(file_type)<<std::endl;
      CommAbort(-1); //TODO
  }
}



int main(int argc, char **argv){
  CommInit(&argc,&argv);

  if(CommRank()==0){
    std::string many_or_one;
    std::string retention;
    std::string input_file;
    std::string output_name;
    int         bwidth    = -1;
    int         bheight   = -1;
    int         flipH     = false;
    int         flipV     = false;

    Timer timer_master;
    timer_master.start();

    std::string analysis = PrintRichdemHeader(argc,argv);

    std::cerr<<"A "<<"A Barnes (2016) Parallel Priority-Flood" <<std::endl;
    std::cerr<<"C "<<"C Barnes, R., 2016. \"Parallel priority-flood depression filling for trillion cell digital elevation models on desktops or clusters\". Computers & Geosciences. doi:10.1016/j.cageo.2016.07.001"<<std::endl;

    std::string help=
    #include "help.txt"
    ;

    try{
      for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--bwidth")==0 || strcmp(argv[i],"-w")==0){
          if(i+1==argc)
            throw std::invalid_argument("-w followed by no argument.");
          bwidth = std::stoi(argv[i+1]);
          if(bwidth<300 && bwidth!=-1)
            throw std::invalid_argument("Width must be at least 500.");
          i++;
          continue;
        } else if(strcmp(argv[i],"--bheight")==0 || strcmp(argv[i],"-h")==0){
          if(i+1==argc)
            throw std::invalid_argument("-h followed by no argument.");
          bheight = std::stoi(argv[i+1]);
          if(bheight<300 && bheight!=-1)
            throw std::invalid_argument("Height must be at least 500.");
          i++;
          continue;
        } else if(strcmp(argv[i],"--help")==0){
          std::cerr<<help<<std::endl;
          int good_to_go=0;
          CommBroadcast(&good_to_go,0);
          CommFinalize();
          return -1;
        } else if(strcmp(argv[i],"--flipH")==0 || strcmp(argv[i],"-H")==0){
          flipH = true;
        } else if(strcmp(argv[i],"--flipV")==0 || strcmp(argv[i],"-V")==0){
          flipV = true;
        } else if(argv[i][0]=='-'){
          throw std::invalid_argument("Unrecognised flag: "+std::string(argv[i]));
        } else if(many_or_one==""){
          many_or_one = argv[i];
        } else if(retention==""){
          retention = argv[i];
        } else if(input_file==""){
          input_file = argv[i];
        } else if(output_name==""){
          output_name = argv[i];
        } else {
          throw std::invalid_argument("Too many arguments.");
        }
      }
      if(many_or_one=="" || retention=="" || input_file=="" || output_name=="")
        throw std::invalid_argument("Too few arguments.");
      if(retention[0]=='@' && !(retention=="@evict" || retention=="@retain"))
        throw std::invalid_argument("Retention must be @evict or @retain or a path.");
      if(many_or_one!="many" && many_or_one!="one")
        throw std::invalid_argument("Must specify many or one.");
      if(CommSize()==1) //TODO: Invoke a special "one-process mode?"
        throw std::invalid_argument("Must run program with at least two processes!");
      if( !((output_name.find("%f")==std::string::npos) ^ (output_name.find("%n")==std::string::npos)) )
        throw std::invalid_argument("Output filename must indicate either file number (%n) or name (%f).");
      if(retention[0]!='@' && retention.find("%n")==std::string::npos && retention.find("%f")==std::string::npos)
        throw std::invalid_argument("Retention filename must indicate file number with '%n' or '%f'.");
      if(retention==output_name)
        throw std::invalid_argument("Retention and output filenames must differ.");
    } catch (const std::invalid_argument &ia){
      std::string output_err;
      if(ia.what()==std::string("stoi"))
        output_err = "Invalid width or height.";
      else
        output_err = ia.what();

      std::cerr<<"parallel_pflood.exe [--flipV] [--flipH] [--bwidth #] [--bheight #] <many/one> <retention> <input> <output>"<<std::endl;
      std::cerr<<"\tUse '--help' to show help."<<std::endl;

      std::cerr<<"E "<<output_err<<std::endl;

      int good_to_go=0;
      CommBroadcast(&good_to_go,0);
      CommFinalize();
      return -1;
    }

    int good_to_go = 1;
    std::cerr<<"c Running with = "           <<CommSize()<<" processes"<<std::endl;
    std::cerr<<"c Many or one = "            <<many_or_one<<std::endl;
    std::cerr<<"c Input file = "             <<input_file<<std::endl;
    std::cerr<<"c Retention strategy = "     <<retention <<std::endl;
    std::cerr<<"c Block width = "            <<bwidth    <<std::endl;
    std::cerr<<"c Block height = "           <<bheight   <<std::endl;
    std::cerr<<"c Flip horizontal = "        <<flipH     <<std::endl;
    std::cerr<<"c Flip vertical = "          <<flipV     <<std::endl;
    std::cerr<<"c World Size = "             <<CommSize()<<std::endl;
    CommBroadcast(&good_to_go,0);
    Preparer(many_or_one, retention, input_file, output_name, bwidth, bheight, flipH, flipV, analysis);

    timer_master.stop();
    std::cerr<<"t Total wall-time = "<<timer_master.accumulated()<<" s"<<std::endl;

  } else {
    int good_to_go;
    CommBroadcast(&good_to_go,0);
    if(good_to_go){
      GDALDataType file_type;
      CommBroadcast(&file_type,0);
      switch(file_type){
        case GDT_Byte:
          Consumer<uint8_t >();break;
        case GDT_UInt16:
          Consumer<uint16_t>();break;
        case GDT_Int16:
          Consumer<int16_t >();break;
        case GDT_UInt32:
          Consumer<uint32_t>();break;
        case GDT_Int32:
          Consumer<int32_t >();break;
        case GDT_Float32:
          Consumer<float   >();break;
        case GDT_Float64:
          Consumer<double  >();break;
        default:
          return -1;
      }
    }
  }

  CommFinalize();

  return 0;
}