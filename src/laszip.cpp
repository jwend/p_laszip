/*
===============================================================================

  FILE:  laszip.cpp

  
  CONTENTS:
  
    This tool compresses and uncompresses LiDAR data in the LAS format to our
    losslessly compressed LAZ format.

  PROGRAMMERS:
  
    martin.isenburg@rapidlasso.com  -  http://rapidlasso.com
  
  COPYRIGHT:
  
    (c) 2007-2015, martin isenburg, rapidlasso - fast tools to catch reality

    This is free software; you can redistribute and/or modify it under the
    terms of the GNU Lesser General Licence as published by the Free Software
    Foundation. See the LICENSE.txt file for more information.

    This software is distributed WITHOUT ANY WARRANTY and without even the
    implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  
  CHANGE HISTORY:
  
    29 March 2015 -- using LASwriterCompatible for LAS 1.4 compatibility mode  
    9 September 2014 -- prototyping forward-compatible coding of LAS 1.4 points  
    5 August 2011 -- possible to add/change projection info in command line
    23 June 2011 -- turned on LASzip version 2.0 compressor with chunking 
    17 May 2011 -- enabling batch processing with wildcards or multiple file names
    25 April 2011 -- added chunking for random access decompression
    23 January 2011 -- added LASreadOpener and LASwriteOpener 
    16 December 2010 -- updated to use the new library
    12 March 2009 -- updated to ask for input if started without arguments 
    17 September 2008 -- updated to deal with LAS format version 1.2
    14 February 2007 -- created after picking flowers for the Valentine dinner
  
===============================================================================
*/

#include "debug.hpp"

#include <time.h>
#include <stdlib.h>
#include <typeinfo>

#include <map>
using namespace std;

#include "lasreader.hpp"
#include "laswriter.hpp"
#include "laswritercompatible.hpp"
#include "laswaveform13reader.hpp"
#include "laswaveform13writer.hpp"
#include "bytestreamin.hpp"
#include "bytestreamout_array.hpp"
#include "bytestreamin_array.hpp"
#include "geoprojectionconverter.hpp"
#include "lasindex.hpp"
#include "lasquadtree.hpp"
#include "laswritepoint.hpp"
#include "arithmeticencoder.hpp"

#include "mpi.h"

class OffsetSize
{
public:
  OffsetSize(U64 o, U32 s) { offset = o; size = s; };
  U64 offset;
  U32 size;
};

typedef map<U64, OffsetSize> my_offset_size_map;

void usage(bool error=false, bool wait=false)
{
  fprintf(stderr,"usage:\n");
  fprintf(stderr,"laszip *.las\n");
  fprintf(stderr,"laszip *.laz\n");
  fprintf(stderr,"laszip *.txt -iparse xyztiarn\n");
  fprintf(stderr,"laszip lidar.las\n");
  fprintf(stderr,"laszip lidar.laz -v\n");
  fprintf(stderr,"laszip -i lidar.las -o lidar_zipped.laz\n");
  fprintf(stderr,"laszip -i lidar.laz -o lidar_unzipped.las\n");
  fprintf(stderr,"laszip -i lidar.las -stdout -olaz > lidar.laz\n");
  fprintf(stderr,"laszip -stdin -o lidar.laz < lidar.las\n");
  fprintf(stderr,"laszip -h\n");
  if (wait)
  {
    fprintf(stderr,"<press ENTER>\n");
    getc(stdin);
  }
  exit(error);
}

static void byebye(bool error=false, bool wait=false)
{
  if (wait)
  {
    fprintf(stderr,"<press ENTER>\n");
    getc(stdin);
  }
  MPI_Finalize();
  exit(error);
}

static double taketime()
{
  return (double)(clock())/CLOCKS_PER_SEC;
}

#ifdef COMPILE_WITH_GUI
extern int laszip_gui(int argc, char *argv[], LASreadOpener* lasreadopener);
#endif

#ifdef COMPILE_WITH_MULTI_CORE
extern int laszip_multi_core(int argc, char *argv[], GeoProjectionConverter* geoprojectionconverter, LASreadOpener* lasreadopener, LASwriteOpener* laswriteopener, int cores);
#endif

int main(int argc, char *argv[])
{
  MPI_Init(&argc, &argv);
  int i;
  BOOL dry = FALSE;
#ifdef COMPILE_WITH_GUI
  BOOL gui = FALSE;
#endif
#ifdef COMPILE_WITH_MULTI_CORE
  I32 cores = 1;
#endif
  BOOL verbose = FALSE;
  bool waveform = false;
  bool waveform_with_map = false;
  bool report_file_size = false;
  bool check_integrity = false;
  I32 end_of_points = -1;
  bool projection_was_set = false;
  bool format_not_specified = false;
  BOOL lax = FALSE;
  BOOL append = FALSE;
  BOOL remain_compatible = FALSE;
  BOOL move_CRS = FALSE;
  BOOL move_all = FALSE;
  F32 tile_size = 100.0f;
  U32 threshold = 1000;
  U32 minimum_points = 100000;
  I32 maximum_intervals = -20;
  double start_time = 0.0;
  double total_start_time = 0;

  LASreadOpener lasreadopener;
  GeoProjectionConverter geoprojectionconverter;
  LASwriteOpener laswriteopener;

  if (argc == 1)
  {
#ifdef COMPILE_WITH_GUI
    return laszip_gui(argc, argv, 0);
#else
    fprintf(stderr,"%s is better run in the command line\n", argv[0]);
    char file_name[256];
    fprintf(stderr,"enter input file: "); fgets(file_name, 256, stdin);
    file_name[strlen(file_name)-1] = '\0';
    lasreadopener.set_file_name(file_name);
    fprintf(stderr,"enter output file: "); fgets(file_name, 256, stdin);
    file_name[strlen(file_name)-1] = '\0';
    laswriteopener.set_file_name(file_name);
#endif
  }
  else
  {
    for (i = 1; i < argc; i++)
    {
      if (argv[i][0] == '�') argv[i][0] = '-';
    }
    if (!geoprojectionconverter.parse(argc, argv)) byebye(true);
    if (!lasreadopener.parse(argc, argv)) byebye(true);
    if (!laswriteopener.parse(argc, argv)) byebye(true);
  }

  for (i = 1; i < argc; i++)
  {
    if (argv[i][0] == '\0')
    {
      continue;
    }
    else if (strcmp(argv[i],"-h") == 0 || strcmp(argv[i],"-help") == 0)
    {
      fprintf(stderr, "LAStools (by martin@rapidlasso.com) version %d\n", LAS_TOOLS_VERSION);
      usage();
    }
    else if (strcmp(argv[i],"-v") == 0 || strcmp(argv[i],"-verbose") == 0)
    {
      verbose = TRUE;
    }
    else if (strcmp(argv[i],"-version") == 0)
    {
      fprintf(stderr, "LAStools (by martin@rapidlasso.com) version %d\n", LAS_TOOLS_VERSION);
      byebye();
    }
    else if (strcmp(argv[i],"-gui") == 0)
    {
#ifdef COMPILE_WITH_GUI
      gui = TRUE;
#else
      fprintf(stderr, "WARNING: not compiled with GUI support. ignoring '-gui' ...\n");
#endif
    }
    else if (strcmp(argv[i],"-cores") == 0)
    {
#ifdef COMPILE_WITH_MULTI_CORE
      if ((i+1) >= argc)
      {
        fprintf(stderr,"ERROR: '%s' needs 1 argument: number\n", argv[i]);
        usage(true);
      }
      argv[i][0] = '\0';
      i++;
      cores = atoi(argv[i]);
      argv[i][0] = '\0';
#else
      fprintf(stderr, "WARNING: not compiled with multi-core batching. ignoring '-cores' ...\n");
      i++;
#endif
    }
    else if (strcmp(argv[i],"-dry") == 0)
    {
      dry = TRUE;
    }
    else if (strcmp(argv[i],"-lax") == 0)
    {
      lax = TRUE;
    }
    else if (strcmp(argv[i],"-append") == 0)
    {
      append = TRUE;
    }
    else if (strcmp(argv[i],"-remain_compatible") == 0)
    {
      remain_compatible = TRUE;
    }
    else if (strcmp(argv[i],"-move_CRS") == 0)
    {
      move_CRS = TRUE;
    }
    else if (strcmp(argv[i],"-move_all") == 0)
    {
      move_all = TRUE;
    }
    else if (strcmp(argv[i],"-eop") == 0)
    {
      if ((i+1) >= argc)
      {
        fprintf(stderr,"ERROR: '%s' needs 1 argument: char\n", argv[i]);
        usage(true);
      }
      i++;
      end_of_points = atoi(argv[i]);
      if ((end_of_points < 0) || (end_of_points > 255))
      {
        fprintf(stderr,"ERROR: end of points value needs to be between 0 and 255\n");
        usage(true);
      }
    }
    else if (strcmp(argv[i],"-tile_size") == 0)
    {
      if ((i+1) >= argc)
      {
        fprintf(stderr,"ERROR: '%s' needs 1 argument: size\n", argv[i]);
        usage(true);
      }
      i++;
      tile_size = (F32)atof(argv[i]);
    }
    else if (strcmp(argv[i],"-maximum") == 0)
    {
      if ((i+1) >= argc)
      {
        fprintf(stderr,"ERROR: '%s' needs 1 argument: number\n", argv[i]);
        usage(true);
      }
      i++;
      maximum_intervals = atoi(argv[i]);
    }
    else if (strcmp(argv[i],"-minimum") == 0)
    {
      if ((i+1) >= argc)
      {
        fprintf(stderr,"ERROR: '%s' needs 1 argument: number\n", argv[i]);
        usage(true);
      }
      i++;
      minimum_points = atoi(argv[i]);
    }
    else if (strcmp(argv[i],"-threshold") == 0)
    {
      if ((i+1) >= argc)
      {
        fprintf(stderr,"ERROR: '%s' needs 1 argument: value\n", argv[i]);
        usage(true);
      }
      i++;
      threshold = atoi(argv[i]);
    }
    else if (strcmp(argv[i],"-size") == 0)
    {
      report_file_size = true;
    }
    else if (strcmp(argv[i],"-check") == 0)
    {
      check_integrity = true;
    }
    else if (strcmp(argv[i],"-waveform") == 0 || strcmp(argv[i],"-waveforms") == 0)
    {
      waveform = true;
    }
    else if (strcmp(argv[i],"-waveform_with_map") == 0 || strcmp(argv[i],"-waveforms_with_map") == 0)
    {
      waveform = true;
      waveform_with_map = true;
    }
    else if ((argv[i][0] != '-') && (lasreadopener.get_file_name_number() == 0))
    {
      lasreadopener.add_file_name(argv[i]);
      argv[i][0] = '\0';
    }
    else
    {
      fprintf(stderr, "ERROR: cannot understand argument '%s'\n", argv[i]);
      usage(true);
    }
  }

#ifdef COMPILE_WITH_GUI
  if (gui)
  {
    return laszip_gui(argc, argv, &lasreadopener);
  }
#endif

#ifdef COMPILE_WITH_MULTI_CORE
  if (cores > 1)
  {
    if (lasreadopener.get_file_name_number() < 2)
    {
      fprintf(stderr,"WARNING: only %u input files. ignoring '-cores %d' ...\n", lasreadopener.get_file_name_number(), cores);
    }
    else if (lasreadopener.is_merged())
    {
      fprintf(stderr,"WARNING: input files merged on-the-fly. ignoring '-cores %d' ...\n", cores);
    }
    else
    {
      return laszip_multi_core(argc, argv, &geoprojectionconverter, &lasreadopener, &laswriteopener, cores);
    }
  }
#endif

  // check input

  if (!lasreadopener.active())
  {
    fprintf(stderr,"ERROR: no input specified\n");
    usage(true, argc==1);
  }

  // check output

  if (laswriteopener.is_piped())
  {
    if (lax)
    {
      fprintf(stderr,"WARNING: disabling LAX generation for piped output\n");
      lax = FALSE;
      append = FALSE;
    }
  }

  // make sure we do not corrupt the input file

  if (lasreadopener.get_file_name() && laswriteopener.get_file_name() && (strcmp(lasreadopener.get_file_name(), laswriteopener.get_file_name()) == 0))
  {
    fprintf(stderr, "ERROR: input and output file name are identical\n");
    usage(true);
  }

  // check if projection info was set in the command line

  int number_of_keys;
  GeoProjectionGeoKeys* geo_keys = 0;
  int num_geo_double_params;
  double* geo_double_params = 0;

  if (geoprojectionconverter.has_projection())
  {
    projection_was_set = geoprojectionconverter.get_geo_keys_from_projection(number_of_keys, &geo_keys, num_geo_double_params, &geo_double_params);
  }

  // check if the output format was *not* specified in the command line

  format_not_specified = !laswriteopener.format_was_specified();

  if (verbose) total_start_time = taketime();

  // loop over multiple input files

  while (lasreadopener.active())
  {
    if (verbose) start_time = taketime();

    // open lasreader

    LASreader* lasreader = lasreadopener.open();

    if (lasreader == 0)
    {
      fprintf(stderr, "ERROR: could not open lasreader\n");
      usage(true, argc==1);
    }

    // switch

    if (report_file_size)
    {
      // maybe only report uncompressed file size
      I64 uncompressed_file_size = lasreader->npoints * lasreader->header.point_data_record_length + lasreader->header.offset_to_point_data;
      if (uncompressed_file_size == (I64)((U32)uncompressed_file_size))
        fprintf(stderr,"uncompressed file size is %u bytes or %.2f MB for '%s'\n", (U32)uncompressed_file_size, (F64)uncompressed_file_size/1024.0/1024.0, lasreadopener.get_file_name());
      else
        fprintf(stderr,"uncompressed file size is %.2f MB or %.2f GB for '%s'\n", (F64)uncompressed_file_size/1024.0/1024.0, (F64)uncompressed_file_size/1024.0/1024.0/1024.0, lasreadopener.get_file_name());
    }
    else if (dry || check_integrity)
    {
      // maybe only a dry read pass
      start_time = taketime();
      while (lasreader->read_point());
      if (check_integrity)
      {
        if (lasreader->p_count != lasreader->npoints)
        {
#ifdef _WIN32
          fprintf(stderr,"FAILED integrity check for '%s' after %I64d of %I64d points\n", lasreadopener.get_file_name(), lasreader->p_count, lasreader->npoints);
#else
          fprintf(stderr,"FAILED integrity check for '%s' after %lld of %lld points\n", lasreadopener.get_file_name(), lasreader->p_count, lasreader->npoints);
#endif
        }
        else
        {
          fprintf(stderr,"SUCCESS for '%s'\n", lasreadopener.get_file_name());
        }
      }
      else
      {
        fprintf(stderr,"needed %g secs to read '%s'\n", taketime()-start_time, lasreadopener.get_file_name());
      }
    }
    else
    {
      I64 start_of_waveform_data_packet_record = 0;

      // create output file name if no output was specified 
      if (!laswriteopener.active())
      {
        if (lasreadopener.get_file_name() == 0)
        {
          fprintf(stderr,"ERROR: no output specified\n");
          usage(true, argc==1);
        }
        laswriteopener.set_force(TRUE);
        if (format_not_specified)
        {
          if (lasreader->get_format() == LAS_TOOLS_FORMAT_LAZ)
          {
            laswriteopener.set_format(LAS_TOOLS_FORMAT_LAS);
          }
          else
          {
            laswriteopener.set_format(LAS_TOOLS_FORMAT_LAZ);
          }
        }
        laswriteopener.make_file_name(lasreadopener.get_file_name(), -2);
      }

      // maybe set projection

      if (projection_was_set)
      {
        lasreader->header.set_geo_keys(number_of_keys, (LASvlr_key_entry*)geo_keys);
        if (geo_double_params)
        {
          lasreader->header.set_geo_double_params(num_geo_double_params, geo_double_params);
        }
        else
        {
          lasreader->header.del_geo_double_params();
        }
        lasreader->header.del_geo_ascii_params();
      }

      //********************** INITIAL WAVEFORM CODE *****************************************
      // almost never open laswaveform13reader and laswaveform13writer (-:

      LASwaveform13reader* laswaveform13reader = 0;
      LASwaveform13writer* laswaveform13writer = 0;

      if (waveform)
      {
        laswaveform13reader = lasreadopener.open_waveform13(&lasreader->header);
        if (laswaveform13reader)
        {
          // switch compression on/off
          U8 compression_type = (laswriteopener.get_format() == LAS_TOOLS_FORMAT_LAZ ? 1 : 0);
          for (i = 0; i < 255; i++) if (lasreader->header.vlr_wave_packet_descr[i]) lasreader->header.vlr_wave_packet_descr[i]->setCompressionType(compression_type);
          // create laswaveform13writer
          laswaveform13writer = laswriteopener.open_waveform13(&lasreader->header);
          if (laswaveform13writer == 0)
          {
            delete [] laswaveform13reader;
            laswaveform13reader = 0;
            waveform = 0;
            // switch compression on/off back
            U8 compression_type = (laswriteopener.get_format() == LAS_TOOLS_FORMAT_LAZ ? 0 : 1);
            for (i = 0; i < 255; i++) if (lasreader->header.vlr_wave_packet_descr[i]) lasreader->header.vlr_wave_packet_descr[i]->setCompressionType(compression_type);
          }
        }
        else
        {
          waveform = false;
        }
      }

      // special check for LAS 1.3+ files that contain waveform data

      if ((lasreader->header.version_major == 1) && (lasreader->header.version_minor >= 3))
      {
        if (lasreader->header.global_encoding & 2) // if bit # 1 is set we have internal waveform data
        {
          lasreader->header.global_encoding &= ~((U16)2); // remove internal bit
          if (lasreader->header.start_of_waveform_data_packet_record) // offset to 
          {
            start_of_waveform_data_packet_record = lasreader->header.start_of_waveform_data_packet_record;
            lasreader->header.start_of_waveform_data_packet_record = 0;
            lasreader->header.global_encoding |= ((U16)4); // set external bit
          }
        }
      }
      // *************************************** INITIAL WAVEFORM CODE END *******************************************
      I64 bytes_written = 0;

      // open laswriter

      LASwriter* laswriter = 0;
      
      if (lasreader->header.point_data_format > 5)
      {
        LASwriterCompatibleDown* laswritercompatibledown = new LASwriterCompatibleDown();
        if (laswritercompatibledown->open(&lasreader->header, &laswriteopener, move_CRS, move_all))
        {
          laswriter = laswritercompatibledown;
        }
        else
        {
          delete laswritercompatibledown;
          fprintf(stderr, "ERROR: could not open laswritercompatibledown\n");
        }
      }
      else if (!remain_compatible && (lasreader->header.point_data_format != 0) && (lasreader->header.point_data_format != 2) && lasreader->header.get_vlr("lascompatible", 22204) && (lasreader->header.get_attribute_index("LAS 1.4 scan angle") >= 0) && (lasreader->header.get_attribute_index("LAS 1.4 extended returns") >= 0) && (lasreader->header.get_attribute_index("LAS 1.4 classification") >= 0) && (lasreader->header.get_attribute_index("LAS 1.4 flags and channel") >= 0))
      {
        LASwriterCompatibleUp* laswritercompatibleup = new LASwriterCompatibleUp();
        if (laswritercompatibleup->open(&lasreader->header, &laswriteopener))
        {
          laswriter = laswritercompatibleup;
        }
        else
        {
          delete laswritercompatibleup;
          fprintf(stderr, "ERROR: could not open laswritercompatibleup\n");
        }
      }
      else
      {
        // jdw, mpi, use nil writer for first read/write iteration, which just calculates point write offsets for each process
        laswriteopener.set_use_nil(TRUE);
        laswriter = laswriteopener.open(&lasreader->header);
      }

      if (laswriter == 0)
      {
        fprintf(stderr, "ERROR: could not open laswriter\n");
        usage(true, argc==1);
      }

      // should we also deal with waveform data

      if (waveform) // ************************** START WAVEFORM *********************************************
      {
        U8 compression_type = (laswaveform13reader->is_compressed() ? 1 : 0);
        for (i = 0; i < 255; i++) if (lasreader->header.vlr_wave_packet_descr[i]) lasreader->header.vlr_wave_packet_descr[i]->setCompressionType(compression_type);

        U64 last_offset = 0;
        U32 last_size = 60;
        U64 new_offset = 0;
        U32 new_size = 0;
        U32 waves_written = 0;
        U32 waves_referenced = 0;

        my_offset_size_map offset_size_map;

        LASindex lasindex;
        if (lax) // should we also create a spatial indexing file
        {
          // setup the quadtree
          LASquadtree* lasquadtree = new LASquadtree;
          lasquadtree->setup(lasreader->header.min_x, lasreader->header.max_x, lasreader->header.min_y, lasreader->header.max_y, tile_size);

          // create lax index
          lasindex.prepare(lasquadtree, threshold);
        }

        // loop over points

        while (lasreader->read_point())
        {
          if (lasreader->point.wavepacket.getIndex()) // if point is attached to a waveform
          {
            waves_referenced++;
            if (lasreader->point.wavepacket.getOffset() == last_offset)
            {
              lasreader->point.wavepacket.setOffset(new_offset);
              lasreader->point.wavepacket.setSize(new_size);
            }
            else if (lasreader->point.wavepacket.getOffset() > last_offset)
            {
              if (lasreader->point.wavepacket.getOffset() > (last_offset + last_size))
              {
                if (!waveform_with_map)
                {
                  fprintf(stderr,"WARNING: gap in waveform offsets.\n");
#ifdef _WIN32
                  fprintf(stderr,"WARNING: last offset plus size was %I64d but new offset is %I64d (for point %I64d)\n", (last_offset + last_size), lasreader->point.wavepacket.getOffset(), lasreader->p_count);
#else
                  fprintf(stderr,"WARNING: last offset plus size was %lld but new offset is %lld (for point %lld)\n", (last_offset + last_size), lasreader->point.wavepacket.getOffset(), lasreader->p_count);
#endif
                }
              }
              waves_written++;
              last_offset = lasreader->point.wavepacket.getOffset();
              last_size = lasreader->point.wavepacket.getSize();
              laswaveform13reader->read_waveform(&lasreader->point);
              laswaveform13writer->write_waveform(&lasreader->point, laswaveform13reader->samples);
              new_offset = lasreader->point.wavepacket.getOffset();
              new_size = lasreader->point.wavepacket.getSize();
              if (waveform_with_map)
              {
                offset_size_map.insert(my_offset_size_map::value_type(last_offset, OffsetSize(new_offset,new_size)));
              }
            }
            else
            {
              if (waveform_with_map)
              {
                my_offset_size_map::iterator map_element;
                map_element = offset_size_map.find(lasreader->point.wavepacket.getOffset());
                if (map_element == offset_size_map.end())
                {
                  waves_written++;
                  last_offset = lasreader->point.wavepacket.getOffset();
                  last_size = lasreader->point.wavepacket.getSize();
                  laswaveform13reader->read_waveform(&lasreader->point);
                  laswaveform13writer->write_waveform(&lasreader->point, laswaveform13reader->samples);
                  new_offset = lasreader->point.wavepacket.getOffset();
                  new_size = lasreader->point.wavepacket.getSize();
                  offset_size_map.insert(my_offset_size_map::value_type(last_offset, OffsetSize(new_offset,new_size)));
                }
                else
                {
                  lasreader->point.wavepacket.setOffset((*map_element).second.offset);
                  lasreader->point.wavepacket.setSize((*map_element).second.size);
                }
              }
              else
              {
                fprintf(stderr,"ERROR: waveform offsets not in monotonically increasing order.\n");
#ifdef _WIN32
                fprintf(stderr,"ERROR: last offset was %I64d but new offset is %I64d (for point %I64d)\n", last_offset, lasreader->point.wavepacket.getOffset(), lasreader->p_count);
#else
                fprintf(stderr,"ERROR: last offset was %lld but new offset is %lld (for point %lld)\n", last_offset, lasreader->point.wavepacket.getOffset(), lasreader->p_count);
#endif
                fprintf(stderr,"ERROR: use option '-waveforms_with_map' to compress.\n");
                byebye(true, argc==1);
              }
            }
          }
          laswriter->write_point(&lasreader->point);
          if (lax)
          {
            lasindex.add(lasreader->point.get_x(), lasreader->point.get_y(), (U32)(laswriter->p_count));
          }
          if (!lasreadopener.is_header_populated())
          {
            laswriter->update_inventory(&lasreader->point);
          }
        }

        if (verbose && ((laswriter->p_count % 1000000) == 0)) fprintf(stderr,"written %d referenced %d of %d points\n", waves_written, waves_referenced, (I32)laswriter->p_count);

        if (!lasreadopener.is_header_populated())
        {
          laswriter->update_header(&lasreader->header, TRUE);
        }

        // flush the writer
        bytes_written = laswriter->close();

        if (lax)
        {
          // adaptive coarsening
          lasindex.complete(minimum_points, maximum_intervals);

          if (append)
          {
            // append lax to file
            lasindex.append(laswriteopener.get_file_name());
          }
          else
          {
            // write lax to file
            lasindex.write(laswriteopener.get_file_name());
          }
        }
      }  // **************************** END WAVEFORM ********************************************************
      else
      {
        // loop over points
        if (lasreadopener.is_header_populated())
        {
          if (lax) // should we also create a spatial indexing file
          {
            // setup the quadtree
            LASquadtree* lasquadtree = new LASquadtree;
            lasquadtree->setup(lasreader->header.min_x, lasreader->header.max_x, lasreader->header.min_y, lasreader->header.max_y, tile_size);

            // create lax index
            LASindex lasindex;
            lasindex.prepare(lasquadtree, threshold);
  
            // compress points and add to index
            while (lasreader->read_point())
            {
              lasindex.add(lasreader->point.get_x(), lasreader->point.get_y(), (U32)(laswriter->p_count));
              laswriter->write_point(&lasreader->point);
            }

            // flush the writer
            bytes_written = laswriter->close();

            // adaptive coarsening
            lasindex.complete(minimum_points, maximum_intervals);

            if (append)
            {
              // append lax to file
              lasindex.append(laswriteopener.get_file_name());
            }
            else
            {
              // write lax to file
              lasindex.write(laswriteopener.get_file_name());
            }
          }
          else // lax is null
          {
            if (end_of_points > -1)
            {
              U8 point10[20];
              memset(point10, end_of_points, 20);

              if (verbose) fprintf(stderr, "writing with end_of_points value %d\n", end_of_points);

              while (lasreader->read_point())
              {
                if (memcmp(point10, &lasreader->point, 20) == 0)
                {
                  break;
                }
                laswriter->write_point(&lasreader->point);
                laswriter->update_inventory(&lasreader->point);
              }
              laswriter->update_header(&lasreader->header, TRUE);
            }
            else // end_of_points <= -1
            {
// Start of straight las -> laz file conversion case, no waveform, lax, or end_of_points parmaeter


              int process_count, rank;

              MPI_Comm_size(MPI_COMM_WORLD, &process_count);
              MPI_Comm_rank(MPI_COMM_WORLD, &rank);



              // ***** Determine the start and stop points for this process *****

              I64 process_points;
              I64 point_start;
              I64 point_end;

              if (lasreader->header.laszip == NULL ) // las -> laz
              {
                // Divide up points on chuck_size boundaries
                // (Assumes chunks >= process_count for now)
                I64 chunk_size = laswriteopener.get_chunk_size ();
                I64 chunks = lasreader->npoints / chunk_size;
                I64 left_over_points = lasreader->npoints % chunk_size;
                I64 process_chunks = chunks / process_count;
                I64 left_over_chunks = chunks % process_count;
                I64 *all_process_chunks = (I64 *) malloc (sizeof(I64) * process_count);
                for (int i = 0; i < process_count; i++)
                {
                  all_process_chunks[i] = process_chunks;
                  if (left_over_chunks)
                  {
                    all_process_chunks[i]++;
                    left_over_chunks--;
                  }
                }
                I64 *all_point_start = (I64 *) malloc (sizeof(I64) * process_count);
                I64 cur_point_start = 0;
                for (int i = 0; i < process_count; i++)
                {
                  all_point_start[i] = cur_point_start;
                  cur_point_start += all_process_chunks[i] * chunk_size;
                }

                process_points = all_process_chunks[rank] * chunk_size;
                point_start = all_point_start[rank];
                point_end = point_start + process_points;
                // Last process gets the left over points, this is only process with
                // process_points that are not a multiple of chunk_size
                if (rank == process_count - 1)
                  point_end += left_over_points;

                dbg(3, "rank %i point_start %lli point_end %lli", rank, point_start, point_end);
              }
              else // laz -> las
              {
                I64 left_over_points = lasreader->npoints % process_count;
                process_points = lasreader->npoints / process_count;
                point_start = rank*process_points;
                point_end =  point_start + process_points;
                if(rank == process_count-1) point_end += left_over_points;

              }

              // **************** First iteration to determine point write offsets
              I64 point_start_offset, point_end_offset;
              point_start_offset = laswriter->get_stream()->tell();
              lasreader->seek(point_start);
              dbg(3, "rank %i point_start %lli point_end %lli", rank, point_start, point_end);
              while (lasreader->read_point())
              {
                laswriter->write_point(&lasreader->point);
                if(laswriter->p_count == point_end-point_start)
                {
                  break;
                }
              }
              MPI_Barrier(MPI_COMM_WORLD);
              if (lasreader->header.laszip == NULL) // las -> laz
              {
                laswriter->get_writer ()->enc->done ();
                laswriter->get_writer ()->add_chunk_to_table ();
              }
              MPI_Barrier(MPI_COMM_WORLD);

              point_end_offset = laswriter->get_stream()->tell();
              I64 point_bytes_written = point_end_offset - point_start_offset;

              // **** Gather the point_bytes_written by the writers of all processes
              I64 *all_point_bytes_written = (I64 *) malloc(sizeof(I64) * process_count);
              MPI_Barrier(MPI_COMM_WORLD);
              dbg(3, "rank %i  point_bytes_written %lli point_start_offset %lli point_end_offset %lli", rank, point_bytes_written, point_start_offset,point_end_offset);
              MPI_Barrier(MPI_COMM_WORLD);

              MPI_Allgather(&point_bytes_written, 1, MPI_LONG_LONG_INT, all_point_bytes_written, 1,
                  MPI_LONG_LONG_INT, MPI_COMM_WORLD);
              for(int i = 0; i<process_count; i++ )
              {
                if(rank==0) dbg(3, "rank %i  all_point_bytes_written %lli", i, all_point_bytes_written[i]);

              }

              // **** Open the output file
              MPI_Barrier(MPI_COMM_WORLD);
              laswriteopener.set_use_nil(FALSE);
              laswriter = laswriteopener.open(&lasreader->header);
              MPI_Barrier(MPI_COMM_WORLD);

              // **** Calculate the write point offset for this process
              I64 write_point_offset = laswriter->get_stream()->tell();
              for(int i=0; i< rank; i++)
              {
                write_point_offset += all_point_bytes_written[i];
              }
              // Iterate over points a second time, this time write compressed bytes to file
              laswriter->get_stream()->seek(write_point_offset);
              if (lasreader->header.laszip == NULL ) // las -> laz
              {
                laswriter->get_writer()->chunk_start_position = laswriter->get_stream()->tell();
              }
              lasreader->seek(point_start);
              dbg(3, "write point loop start, rank %i, point_start %lli, write_point_offset %lli", rank, point_start, write_point_offset);
              while (lasreader->read_point())
              {
                laswriter->write_point(&lasreader->point);
                if(laswriter->p_count == point_end-point_start)
                {
                  break;
                }
              }
              MPI_Barrier(MPI_COMM_WORLD);
              if (lasreader->header.laszip == NULL) // las -> laz
              {
                laswriter->get_writer ()->enc->done ();
                laswriter->get_writer ()->add_chunk_to_table ();
              }
              MPI_Barrier(MPI_COMM_WORLD);

              if (lasreader->header.laszip == NULL) // las -> laz
              {
                // **** At this point all processes have written their point ranges
                // **** Now the last process gathers and writes the number_chunks chunk_bytes
                // **** Note that chunk_sizes in NOT populated or written
                U32 *number_chunks = (U32*) malloc (sizeof(U32) * process_count);
                MPI_Gather (&(laswriter->get_writer ()->number_chunks), 1, MPI_UNSIGNED, number_chunks, 1, MPI_UNSIGNED, process_count - 1, MPI_COMM_WORLD);
                U32 number_chunks_total = 0;
                if (rank == process_count - 1)
                {
                  for (int i = 0; i < process_count; i++)
                  {
                    number_chunks_total += number_chunks[i];
                  }
                }
                //U32 *chunk_sizes = (U32*)malloc(sizeof(U32)*number_chunks_total);
                U32 *chunk_bytes = (U32*) malloc (sizeof(U32) * number_chunks_total);

                // MPI_Send(&(laswriter->get_writer()->chunk_sizes), laswriter->get_writer()->number_chunks, MPI_UNSIGNED, process_count-1, 1, MPI_COMM_WORLD);
                MPI_Send (laswriter->get_writer ()->chunk_bytes, laswriter->get_writer ()->number_chunks, MPI_UNSIGNED, process_count - 1, 2, MPI_COMM_WORLD);

                U32 *number_chunks_offsets = (U32 *) malloc (sizeof(U32) * process_count);
                U32 current_offset = 0;
                for (int i = 0; i < process_count; i++)
                {
                  number_chunks_offsets[i] = current_offset;
                  current_offset += number_chunks[i];
                }

                MPI_Status status;
                if (rank == process_count - 1)
                {
                  for (int i = 0; i < process_count; i++)
                  {
                    //  MPI_Recv(chunk_sizes + number_chunks_offsets[i], number_chunks[i], MPI_UNSIGNED, i, 1, MPI_COMM_WORLD, &status);
                    MPI_Recv (chunk_bytes + number_chunks_offsets[i], number_chunks[i], MPI_UNSIGNED, i, 2, MPI_COMM_WORLD, &status);
                    dbg(3, "rank %i, chunk_offset %u", rank, number_chunks_offsets[i]);
                  }
                }
                MPI_Barrier (MPI_COMM_WORLD);

                // **** Get chunk_table_start_position from, I don't believe this is necessary
                // **** Leave it in for now, 160304
                I64 chunk_table_start_position = 0;
                if (rank == 0)
                  MPI_Send (&(laswriter->get_writer ()->chunk_table_start_position), 1, MPI_LONG_LONG_INT, process_count - 1, 3, MPI_COMM_WORLD);
                if (rank == process_count - 1)
                  MPI_Recv (&chunk_table_start_position, 1, MPI_LONG_LONG_INT, 0, 3, MPI_COMM_WORLD, &status);
                MPI_Barrier (MPI_COMM_WORLD);
                dbg(5, "rank %i, number_chunks_total %u chunk_table_start_position %lli", rank, number_chunks_total, chunk_table_start_position);
                for (int i = 0; i < number_chunks_total; i++)
                {
                  dbg(5, "rank %i, chunk_sizes  chunk_bytes %u", rank, chunk_bytes[i]);
                }
                if (rank == process_count - 1)
                {
                  dbg(3, "rank %i, number_chunks_total %u chunk_table_start_position %lli", rank, laswriter->get_writer()->number_chunks,
                      laswriter->get_writer()->chunk_table_start_position);
                }
                if (rank == process_count - 1)
                {
                  for (int i = 0; i < number_chunks_total; i++)
                  {
                    dbg(5, "rank , chunk_bytes %u", chunk_bytes[i]);
                  }
                }
                // **** Finally the last process writes the aggregated chunk *******
                if (rank == process_count - 1)
                {
                  laswriter->get_writer ()->chunk_table_start_position = chunk_table_start_position;
                  laswriter->get_writer ()->number_chunks = number_chunks_total;
                  //laswriter->get_writer()->chunk_sizes = chunk_sizes;
                  laswriter->get_writer ()->chunk_bytes = chunk_bytes;
                  laswriter->get_writer ()->write_chunk_table ();
                  //laswriter->close();
                }
              }
            }
            // flush the writer
            // TODO, close things cleanly, some of what goes on in close() happens above
            //bytes_written = laswriter->close();
          }
        }
        else
        {
          if (lax && (lasreader->header.min_x < lasreader->header.max_x) && (lasreader->header.min_y < lasreader->header.max_y))
          {
            // setup the quadtree
            LASquadtree* lasquadtree = new LASquadtree;
            lasquadtree->setup(lasreader->header.min_x, lasreader->header.max_x, lasreader->header.min_y, lasreader->header.max_y, tile_size);

            // create lax index
            LASindex lasindex;
            lasindex.prepare(lasquadtree, threshold);
  
            // compress points and add to index
            while (lasreader->read_point())
            {
              lasindex.add(lasreader->point.get_x(), lasreader->point.get_y(), (U32)(laswriter->p_count));
              laswriter->write_point(&lasreader->point);
              laswriter->update_inventory(&lasreader->point);
            }

            // flush the writer
            bytes_written = laswriter->close();

            // adaptive coarsening
            lasindex.complete(minimum_points, maximum_intervals);

            if (append)
            {
              // append lax to file
              lasindex.append(laswriteopener.get_file_name());
            }
            else
            {
              // write lax to file
              lasindex.write(laswriteopener.get_file_name());
            }
          }
          else
          {
            if (end_of_points > -1)
            {
              U8 point10[20];
              memset(point10, end_of_points, 20);

              if (verbose) fprintf(stderr, "writing with end_of_points value %d\n", end_of_points);

              while (lasreader->read_point())
              {
                if (memcmp(point10, &lasreader->point, 20) == 0)
                {
                  break;
                }
                laswriter->write_point(&lasreader->point);
                laswriter->update_inventory(&lasreader->point);
              }
            }
            else
            {
              while (lasreader->read_point())
              {
                laswriter->write_point(&lasreader->point);
                laswriter->update_inventory(&lasreader->point);
              }
            }
          }

          // update the header
          laswriter->update_header(&lasreader->header, TRUE);

          // flush the writer
          bytes_written = laswriter->close();
        }
      }

      //delete laswriter;
  
#ifdef _WIN32
      if (verbose) fprintf(stderr,"%g secs to write %I64d bytes for '%s' with %I64d points of type %d\n", taketime()-start_time, bytes_written, laswriteopener.get_file_name(), lasreader->p_count, lasreader->header.point_data_format);
#else
      if (verbose) fprintf(stderr,"%g secs to write %lld bytes for '%s' with %lld points of type %d\n", taketime()-start_time, bytes_written, laswriteopener.get_file_name(), lasreader->p_count, lasreader->header.point_data_format);
#endif

      if (start_of_waveform_data_packet_record && !waveform)
      {
        lasreader->close(FALSE);
        ByteStreamIn* stream = lasreader->get_stream();
        stream->seek(start_of_waveform_data_packet_record);
        char* wave_form_file_name;
        if (laswriteopener.get_file_name())
        {
          wave_form_file_name = strdup(laswriteopener.get_file_name());
          int len = strlen(wave_form_file_name);
          if (wave_form_file_name[len-3] == 'L')
          {
            wave_form_file_name[len-3] = 'W';
            wave_form_file_name[len-2] = 'D';
            wave_form_file_name[len-1] = 'P';
          }
          else
          {
            wave_form_file_name[len-3] = 'w';
            wave_form_file_name[len-2] = 'd';
            wave_form_file_name[len-1] = 'p';
          }
        }
        else
        {
          wave_form_file_name = strdup("wave_form.wdp");
        }
        FILE* file = fopen(wave_form_file_name, "wb");
        if (file)
        {
          if (verbose) fprintf(stderr,"writing waveforms to '%s'\n", wave_form_file_name);
          try
          {
            int byte;
            while (true)
            {
              byte = stream->getByte();
              fputc(byte, file);
            }
          }
          catch (...)
          {
            fclose(file);
          }
        }
      }

      laswriteopener.set_file_name(0);
      if (format_not_specified)
      {
        laswriteopener.set_format((const CHAR*)NULL);
      }
    }
  
    lasreader->close();

    delete lasreader;
  }

  if (projection_was_set)
  {
    free(geo_keys);
    if (geo_double_params)
    {
      free(geo_double_params);
    }
  }

  if (verbose && lasreadopener.get_file_name_number() > 1) fprintf(stderr,"needed %g sec for %u files\n", taketime()-total_start_time, lasreadopener.get_file_name_number());

  byebye(false, argc==1);

  return 0;
}
