/*
 Copyright 2016 Nervana Systems Inc.
 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
*/

#include <errno.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>

#include "cpio.hpp"
#include "block_loader_cpio_cache.hpp"
#include "file_util.hpp"

using namespace std;
using namespace nervana;

block_loader_cpio_cache::block_loader_cpio_cache(const string& rootCacheDir,
                                                 const string& cache_id,
                                                 const string& version,
                                                 shared_ptr<block_loader> loader) :
    block_loader(loader->block_size()),
    _loader(loader),
    block_count{loader->block_count()},
    cache_owner{false}
{
    invalidate_old_cache(rootCacheDir, cache_id, version);

    _cacheDir = file_util::path_join(rootCacheDir, cache_id + "_" + version);

    if(file_util::make_directory(_cacheDir))
    {
        // If I successfully created the directory then it did not exist.
        // Therefore I am the owner and must write the end-of-data file
        cache_owner = true;
    }

    if(check_if_complete() == false) {
        if(take_ownership() == false) {
            throw std::runtime_error("dataloader cache incomplete, try again later");
        }
    }
}

void block_loader_cpio_cache::load_block(buffer_in_array& dest, uint32_t block_num)
{
    if(load_block_from_cache(dest, block_num)) {
        return;
    } else {
        _loader->load_block(dest, block_num);

        try {
            write_block_to_cache(dest, block_num);

            if(block_num == block_count-1)
            {
                mark_cache_complete();
                release_ownership();
            }
        } catch (std::exception& e) {
            // failure to write block to cache doesn't stop execution, only print an error
            cerr << "ERROR writing block to cache: " << e.what() << endl;
        }
    }
}

bool block_loader_cpio_cache::load_block_from_cache(buffer_in_array& dest, uint32_t block_num)
{
    // load a block from cpio cache into dest.  If file doesn't exist, return false.
    //  If loading from cpio cache was successful return true.
    cpio::file_reader reader;

    if(!reader.open(block_filename(block_num))) {
        // couldn't load the file
        return false;
    }
    // load cpio file into dest one item at a time
    for(int i=0; i < reader.itemCount(); ++i) {
        for (auto d : dest) {
            try {
                reader.read(*d);
            } catch (std::exception& e) {
                d->add_exception(std::current_exception());
            }
        }
    }

    reader.close();

    // cpio file was read successfully, no need to hit primary data
    // source
    return true;
}

void block_loader_cpio_cache::write_block_to_cache(buffer_in_array& buff, uint32_t block_num)
{
    cpio::file_writer writer;
    writer.open(block_filename(block_num));
    writer.write_all_records(buff);
    writer.close();
}

void block_loader_cpio_cache::invalidate_old_cache(const string& rootCacheDir,
                                                 const string& cache_id,
                                                 const string& version)
{
    // remove cache directories that match rootCacheDir and cache_id but not version

    DIR *dir;
    struct dirent *ent;
    if((dir = opendir(rootCacheDir.c_str())) != NULL) {
        while((ent = readdir(dir)) != NULL) {
            if(filename_holds_invalid_cache(ent->d_name, cache_id, version)) {
                file_util::remove_directory(file_util::path_join(rootCacheDir, ent->d_name));
            }
        }
        closedir(dir);
    }
    else {
        throw std::runtime_error("error enumerating old cache in " + rootCacheDir);
    }
}

bool block_loader_cpio_cache::filename_holds_invalid_cache(const string& filename,
                                                        const string& cache_id,
                                                        const string& version)
{
    // in order for `filename` to hold invalid cache, it must begin with
    // `cache_id`, but not contain `version`

    if(filename.find(cache_id) != 0) {
        // filename doesn't start with cache_id, dont remove it
        return false;
    }
    if(filename.find(version) == string::npos) {
        // filename does start with cache_id, but doesnt have version, invalidate
        return true;
    }
    // filename does start with cache_id and does have version, keep, its valid
    return false;
}

string block_loader_cpio_cache::block_filename(uint32_t block_num)
{
    string file = to_string(block_num) + "-" + to_string(_block_size) + ".cpio";
    string rc = file_util::path_join(_cacheDir, file);
    return rc;
}

uint32_t block_loader_cpio_cache::object_count()
{
    return _loader->object_count();
}

bool block_loader_cpio_cache::check_if_complete()
{
    string file = file_util::path_join(_cacheDir, cache_complete_filename);
    return file_util::exists(file);
}

void block_loader_cpio_cache::mark_cache_complete()
{
    string file = file_util::path_join(_cacheDir, cache_complete_filename);
    ofstream f{file};
}

bool block_loader_cpio_cache::take_ownership()
{
    string file = file_util::path_join(_cacheDir, owner_lock_filename);
    ownership_lock = file_util::try_get_lock(file);
    return ownership_lock != -1;
}

void block_loader_cpio_cache::release_ownership()
{
    string file = file_util::path_join(_cacheDir, owner_lock_filename);
    file_util::release_lock(ownership_lock, file);
}

void block_loader_cpio_cache::prefetch_block(uint32_t block_num)
{
    string file = block_filename(block_num);
    if(file_util::exists(file) == false)
    {
        _loader->prefetch_block(block_num);
    }
}
