#include "pvfs.h"
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <set>
#include <cerrno>
#ifdef _WIN32
#include <share.h>
#else
#include <sys/file.h>
#endif
namespace pvfs {

namespace {
// Hold writer ownership on the recording itself for the descriptor's lifetime.
// A sidecar file cannot protect against racing opens or an ignored/deleted lock.
int openRecordingWriter(const char* filename, bool create)
{
    if (!filename) { errno = EINVAL; return PVFS_INVALID_FD; }
#ifdef _WIN32
    int fd = PVFS_INVALID_FD;
    const int flags = _O_RDWR | _O_BINARY | _O_NOINHERIT
        | (create ? _O_CREAT : 0);
    if (_sopen_s(&fd, filename, flags, _SH_DENYWR, _S_IREAD | _S_IWRITE) != 0)
        return PVFS_INVALID_FD;
    // Acquire exclusive write access before truncating an existing recording.
    if (create && _chsize_s(fd, 0) != 0) {
        const int error = errno;
        _close(fd);
        errno = error;
        return PVFS_INVALID_FD;
    }
#else
    const int fd = open(filename, O_RDWR | O_CLOEXEC | (create ? O_CREAT : 0), S_IRUSR | S_IWUSR);
    if (fd == PVFS_INVALID_FD) return fd;
    // POSIX locks are advisory: all PVFS writers must use this entry point.
    if (flock(fd, LOCK_EX | LOCK_NB) != 0 || (create && ftruncate(fd, 0) != 0)) {
        const int error = errno;
        close(fd);
        errno = error;
        return PVFS_INVALID_FD;
    }
#endif
    return fd;
}
}

std::shared_ptr<pvfs::PvfsFile> createVFS(std::uint32_t blockSize)
{
    try {
        auto vfs = std::make_shared<PvfsFile>();
        vfs->version.major = PVFS_VERSION_MAJOR;
        vfs->version.minor = PVFS_VERSION_MINOR;
        vfs->version.revision = PVFS_VERSION_REVISION;
        vfs->blockSize = blockSize;
        vfs->fileMaxCount = ( blockSize ) /
                          ( PVFS_MAX_FILENAME_LENGTH + sizeof ( int64_t ) + sizeof ( int64_t ));
        vfs->treeMaxCount = ( blockSize - sizeof ( int64_t )) /
                                  ( sizeof ( int64_t ) + sizeof ( int64_t ));
        return vfs;
    } catch (const std::bad_alloc& e) {
        std::cerr << "VFS Memory allocation failed: " << e.what() << '\n';
        // Handle allocation error, like returning nullptr or rethrowing
        return nullptr;
    }
}

int PVFS_close(int fd)
{
#ifdef _WIN32
    int fdw = _open_osfhandle((intptr_t)fd, 0);
    return _close(fdw);
#else
    return close(fd);
#endif
}

PvfsFile::~PvfsFile()
{
    if (fd != PVFS_INVALID_FD) {
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        fd = PVFS_INVALID_FD;
    }
}

std::shared_ptr<PvfsFile> create_PVFS_file_structure ( std::uint32_t blockSize )
{
    std::shared_ptr<pvfs::PvfsFile> vfs = createVFS( blockSize );
    if ( !vfs ) return nullptr;

    vfs->fd					= PVFS_INVALID_FD;				// Invalid file handle.
    vfs->tableLoc			= PVFS_HEADER_SIZE;
    vfs->fileBlock			= nullptr;
    vfs->blockSize			= blockSize;
    vfs->fileMaxCount		= 0;
    vfs->treeMaxCount		= 0;
    vfs->block				= nullptr;
    vfs->fileBlock			= nullptr;
    vfs->nextBlock			= PVFS_HEADER_SIZE;

    for ( unsigned long i = 0; i < PVFS_MAX_HANDLES; i++ )
    {
        vfs->fileHandles [ i ] = nullptr;
    }

    vfs->fileMaxCount = ( blockSize ) /
                          ( PVFS_MAX_FILENAME_LENGTH + sizeof ( int64_t ) + sizeof ( int64_t ));
    vfs->treeMaxCount = ( blockSize - sizeof ( int64_t )) /
                                  ( sizeof ( int64_t ) + sizeof ( int64_t ));

    vfs->block				= create_PVFS_block ( vfs );
    vfs->fileBlock			= create_PVFS_block_file ( vfs );
    vfs->fileBlockTemp	= create_PVFS_block_file ( vfs );
    vfs->treeBlockTemp	= create_PVFS_block_tree ( vfs );
    vfs->dataBlockTemp	= create_PVFS_block_data ( vfs );

    return vfs;
}

void PVFS_file_set_blockSize ( std::shared_ptr<PvfsFile> &vfs, std::uint32_t blockSize )
{
    if ( !vfs ) return;
    if ( blockSize == 0 ) return;

    vfs->blockSize		= blockSize;
    vfs->fileMaxCount = ( blockSize ) /
                          ( PVFS_MAX_FILENAME_LENGTH + sizeof ( std::int64_t ) + sizeof ( std::int64_t ));
    vfs->treeMaxCount = ( blockSize - sizeof ( std::int64_t )) /
                                  ( sizeof ( std::int64_t ) + sizeof ( std::int64_t ));

    vfs->block = create_PVFS_block ( vfs );
    vfs->fileBlock = create_PVFS_block_file ( vfs );
    vfs->fileBlockTemp = create_PVFS_block_file(vfs);
    vfs->treeBlockTemp = create_PVFS_block_tree(vfs);
    vfs->dataBlockTemp = create_PVFS_block_data(vfs);

}


std::shared_ptr<PvfsBlock> create_PVFS_block ( std::shared_ptr<PvfsFile> &vfs )
{
    if ( !vfs ) return nullptr;
    std::shared_ptr<PvfsBlock> block = createBlock<PvfsBlock>(vfs->blockSize);
    if ( block == nullptr ) return nullptr;

    block->type		= pvfs::PVFS_BLOCK_TYPE_UNKNOWN;
    block->next		= pvfs::PVFS_INVALID_LOCATION;
    block->prev		= pvfs::PVFS_INVALID_LOCATION;
    block->self		= pvfs::PVFS_INVALID_LOCATION;
    block->count	= 0;
    block->size		= vfs->blockSize;
    return block;
}


std::int64_t PVFS_read_block ( int fd, std::int64_t address, std::shared_ptr<PvfsBlock> &block )
{
    int64_t	counter		=	0;
    int64_t	location	=	0;

    if ( fd == PVFS_INVALID_FD ) return 0;
    if ( !block ) return 0;
    if ( block->size == 0 ) return 0;
    if ( address == PVFS_INVALID_LOCATION ) return 0;

#ifdef _WIN32
    location = _lseeki64 ( fd, address, SEEK_SET );
#else
    location = lseek ( fd, address, SEEK_SET );
#endif
    if(location == -1)return 0;
    counter += PVFS_read_uint8 ( fd, block->type );
    counter += PVFS_read_sint64 ( fd, block->prev );
    counter += PVFS_read_sint64 ( fd, block->self );
    counter += PVFS_read_sint64 ( fd, block->next );
    counter += PVFS_read_sint32 ( fd, block->count );
    // Serialization of a tree/file block can shrink this scratch buffer.
    block->data.resize(block->size);
    unsigned char* buffer = block->data.data();
#ifdef _WIN32
    counter += _read ( fd, buffer, block->size );
#else
    counter += read ( fd, buffer, block->size );
#endif
    return counter;
}

namespace {
bool writeAll(int fd, const void* bytes, size_t size)
{
    auto* data = static_cast<const unsigned char*>(bytes);
    while (size) {
        const auto written = p_write(fd, const_cast<unsigned char*>(data), size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0 || static_cast<size_t>(written) > size) return false;
        data += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}
bool commitFile(int fd)
{
#ifdef _WIN32
    return _commit(fd) == 0;
#else
    return fsync(fd) == 0;
#endif
}
bool seekFile(int fd, int64_t address)
{
#ifdef _WIN32
    return _lseeki64(fd, address, SEEK_SET) == address;
#else
    return lseek(fd, address, SEEK_SET) == address;
#endif
}
}

std::int64_t PVFS_write_block(int fd, std::int64_t address, std::shared_ptr<PvfsBlock>& block)
{
    if (fd == PVFS_INVALID_FD || !block || block->size == 0 || address < 0) return 0;
    if (!seekFile(fd, address)
        || !writeAll(fd, &block->type, sizeof(block->type))
        || !writeAll(fd, &block->prev, sizeof(block->prev))
        || !writeAll(fd, &block->self, sizeof(block->self))
        || !writeAll(fd, &block->next, sizeof(block->next))
        || !writeAll(fd, &block->count, sizeof(block->count))
        || !writeAll(fd, block->data.data(), block->data.size())) return 0;
    return PVFS_BLOCK_HEADER_SIZE + block->data.size();
}

std::shared_ptr<PvfsBlockData> create_PVFS_block_data ( std::shared_ptr<PvfsFile> &vfs )
{
    if ( !vfs ) return nullptr;

    std::shared_ptr<PvfsBlockData> block = createBlock<PvfsBlockData>(vfs->blockSize - sizeof ( std::int64_t ));
    if ( !block ) return nullptr;

    block->type			= PVFS_BLOCK_TYPE_DATA;
    block->prev			= PVFS_INVALID_LOCATION;
    block->self			= PVFS_INVALID_LOCATION;
    block->next			= PVFS_INVALID_LOCATION;
    block->tree			= PVFS_INVALID_LOCATION;
    block->count		= 0;
    block->maxCount	= vfs->blockSize - sizeof ( std::int64_t );
    block->data.resize(block->maxCount);

    return block;
}

std::shared_ptr<pvfs::PvfsBlockTree> create_PVFS_block_tree ( std::shared_ptr<PvfsFile> &vfs )
{
    if ( !vfs ) return nullptr;

        try {
        auto block = std::make_shared<PvfsBlockTree>();
        if ( !block ) return nullptr;
        block->type			= PVFS_BLOCK_TYPE_TREE;
        block->prev			= PVFS_INVALID_LOCATION;
        block->self			= PVFS_INVALID_LOCATION;
        block->next			= PVFS_INVALID_LOCATION;
        block->up			= PVFS_INVALID_LOCATION;
        block->count		= 0;
        block->maxMappings	= vfs->treeMaxCount;
        block->mappings.clear();
        if(vfs->treeMaxCount > 0)block->mappings.resize(vfs->treeMaxCount, PvfsLocationMap());
        return block;
    } catch (const std::bad_alloc& e) {
        std::cerr << "Tree Block Memory allocation failed: " << e.what() << '\n';
        return nullptr;
    }
}


std::shared_ptr<PvfsBlockFile> create_PVFS_block_file ( std::shared_ptr<PvfsFile> &vfs )
{
    if ( !vfs ) return nullptr;

        try {
        auto block = std::make_shared<PvfsBlockFile>();
        if ( !block ) return nullptr;
        block->type			= PVFS_BLOCK_TYPE_FILE;
        block->prev			= PVFS_INVALID_LOCATION;
        block->self			= PVFS_INVALID_LOCATION;
        block->next			= PVFS_INVALID_LOCATION;
        block->count		= 0;
        block->maxFiles	= vfs->fileMaxCount;
        block->files.clear();
        return block;
    } catch (const std::bad_alloc& e) {
        std::cerr << "File Block Memory allocation failed: " << e.what() << '\n';
        return nullptr;
    }
}

std::int32_t PVFS_cast_block_to_data ( std::shared_ptr<PvfsBlock> &block, std::shared_ptr<PvfsBlockData> &data )
{
    if ( !block ) return PVFS_ARG_NULL;
    if ( !data ) return PVFS_ARG_NULL;

    data->next	= block->next;
    data->prev	= block->prev;
    data->self	= block->self;
    data->count	= block->count;
    memcpy(&(data->tree), block->data.data(), sizeof(std::int64_t));
    memcpy ( data->data.data(), block->data.data() + sizeof ( std::int64_t ), data->maxCount );
    return PVFS_OK;
}



std::int32_t PVFS_cast_block_to_tree ( std::shared_ptr<PvfsBlock> &block, std::shared_ptr<PvfsBlockTree> &tree )
{
    if ( !block )	return PVFS_ARG_NULL;
    if ( !tree )	return PVFS_ARG_NULL;

    tree->next	= block->next;
    tree->prev	= block->prev;
    tree->self	= block->self;
    tree->count	= block->count;

memcpy(&(tree->up), block->data.data(), sizeof(std::int64_t));
size_t offset = sizeof(std::int64_t);

tree->mappings.clear();

for (std::int32_t i = 0; i < tree->maxMappings; ++i)
{
    PvfsLocationMap mapping = PvfsLocationMap();
    size_t currentOffset = offset + i * sizeof(PvfsLocationMap);
    memcpy(&mapping, block->data.data() + currentOffset, sizeof(PvfsLocationMap));
    tree->mappings.push_back(mapping);
}
return PVFS_OK;
}

std::int32_t PVFS_cast_block_to_file ( std::shared_ptr<PvfsBlock> &block, std::shared_ptr<PvfsBlockFile> &file )
{

    if ( !block )		return PVFS_ARG_NULL;
    if ( !file )		return PVFS_ARG_NULL;

    file->next	= block->next;
    file->prev	= block->prev;
    file->self	= block->self;

    if ( block->count > file->maxFiles )
    {
        return PVFS_CORRUPTION_DETECTED;
    }

size_t index = 0;
size_t count = 0;

file->files.clear(); // Clear existing entries

    for (std::int32_t i = 0; i < block->count; i++)
    {
        // Check if there's enough data left in block->data for another _PvfsFileEntry
        if (count + sizeof(int64_t) * 2 + PVFS_MAX_FILENAME_LENGTH > block->data.size())
        {
            return PVFS_CORRUPTION_DETECTED;
        }

        PvfsFileEntry fileEntry;

        // Copy startBlock
        memcpy(&fileEntry.startBlock, &block->data[index], sizeof(std::int64_t));
        index += sizeof(std::int64_t);
        count += sizeof(std::int64_t);

        // Copy size
        memcpy(&fileEntry.size, &block->data[index], sizeof(std::int64_t));
        index += sizeof(std::int64_t);
        count += sizeof(std::int64_t);

        // Copy filename
        memcpy(fileEntry.filename, &block->data[index], PVFS_MAX_FILENAME_LENGTH);
        index += PVFS_MAX_FILENAME_LENGTH;
        count += PVFS_MAX_FILENAME_LENGTH;

        // Add the file entry to file->files
        file->files.push_back(fileEntry);
    }
    // count is now derived from files.size() - no need to maintain separately
    return PVFS_OK;
}


std::int32_t PVFS_cast_data_to_block ( std::shared_ptr<PvfsBlockData> &data, std::shared_ptr<PvfsBlock> &block )
{
     if (!block) return PVFS_ARG_NULL;
    if (!data) return PVFS_ARG_NULL;

    block->type = data->type;
    block->next = data->next;
    block->prev = data->prev;
    block->self = data->self;
    block->count = data->count;

    // Prepare the block->data to hold tree plus maxCount elements from data->data
    block->data.clear();
    block->data.resize(sizeof(std::int64_t) + std::min(data->maxCount, static_cast<std::int32_t>(data->data.size())));

    std::uint64_t treeValue = static_cast<std::uint64_t>(data->tree);
    std::copy(reinterpret_cast<std::uint8_t*>(&treeValue), reinterpret_cast<std::uint8_t*>(&treeValue) + sizeof (std::int64_t), block->data.begin());

    if (data->maxCount > 0 && !data->data.empty())
    {
        std::copy(data->data.begin(), data->data.begin() + std::min(data->maxCount, static_cast<std::int32_t>(data->data.size())), block->data.begin() + sizeof(std::int64_t));
    }
    return PVFS_OK;
}

std::int32_t PVFS_cast_tree_to_block(std::shared_ptr<PvfsBlockTree>& tree, std::shared_ptr<PvfsBlock>& block)
{
    if (!block) return PVFS_ARG_NULL;
    if (!tree) return PVFS_ARG_NULL;

    block->type = tree->type;
    block->next = tree->next;
    block->prev = tree->prev;
    block->self = tree->self;
    block->count = tree->count;

    size_t requiredSize = sizeof(int64_t) + (tree->mappings.size() * 2 * sizeof(int64_t));
    block->data.resize(requiredSize);

    auto index = block->data.begin();

    std::copy(reinterpret_cast<uint8_t*>(&tree->up), reinterpret_cast<uint8_t*>(&tree->up) + sizeof(int64_t), index);
    index += sizeof(int64_t);

    for (const auto& mapping : tree->mappings) {
        std::copy(reinterpret_cast<const uint8_t*>(&mapping.address), reinterpret_cast<const uint8_t*>(&mapping.address) + sizeof(int64_t), index);
        index += sizeof(int64_t);
        std::copy(reinterpret_cast<const uint8_t*>(&mapping.blockLoc), reinterpret_cast<const uint8_t*>(&mapping.blockLoc) + sizeof(int64_t), index);
        index += sizeof(int64_t);
    }

    return PVFS_OK;
}

std::int32_t PVFS_cast_file_to_block(std::shared_ptr<PvfsBlockFile>& file, std::shared_ptr<PvfsBlock>& block)
{
    if (!block) return PVFS_ARG_NULL;
    if (!file) return PVFS_ARG_NULL;

    block->type = file->type;
    block->next = file->next;
    block->prev = file->prev;
    block->self = file->self;
    block->count = static_cast<std::int32_t>(file->files.size());

    size_t requiredSize = file->files.size() * (sizeof(std::int64_t) * 2 + PVFS_MAX_FILENAME_LENGTH);
    block->data.resize(requiredSize);

    std::uint8_t* index = block->data.data();

    for (const auto& fileEntry : file->files)
    {
        std::copy(reinterpret_cast<const std::uint8_t*>(&fileEntry.startBlock),
                  reinterpret_cast<const std::uint8_t*>(&fileEntry.startBlock) + sizeof(std::int64_t),
                  index);
        index += sizeof(std::int64_t);

        std::copy(reinterpret_cast<const std::uint8_t*>(&fileEntry.size),
                  reinterpret_cast<const std::uint8_t*>(&fileEntry.size) + sizeof(std::int64_t),
                  index);
        index += sizeof(std::int64_t);

        std::copy(fileEntry.filename,
                  fileEntry.filename + PVFS_MAX_FILENAME_LENGTH,
                  index);
        index += PVFS_MAX_FILENAME_LENGTH;
    }

    return PVFS_OK;
}



std::int32_t PVFS_copy_fileEntry ( PvfsFileEntry* dest, PvfsFileEntry* src )
{
    if ( dest == nullptr ) return PVFS_ARG_NULL;
    if ( src == nullptr ) return PVFS_ARG_NULL;

    dest->size			= src->size;
    dest->startBlock	= src->startBlock;
    memcpy ( dest->filename, src->filename, PVFS_MAX_FILENAME_LENGTH );

    return PVFS_OK;
}


std::int64_t PVFS_read_block_file ( std::shared_ptr<PvfsFile> &vfs, int64_t address, std::shared_ptr<PvfsBlockFile> &block )
{
    int64_t return_address;

    if ( !vfs ) return PVFS_INVALID_LOCATION;
    if ( address == PVFS_INVALID_LOCATION ) return PVFS_INVALID_LOCATION;
    if ( !block ) return PVFS_INVALID_LOCATION;

    return_address = PVFS_INVALID_LOCATION;

    return_address = PVFS_read_block ( vfs->fd, address, vfs->block );
    if (return_address != PVFS_BLOCK_HEADER_SIZE + vfs->blockSize
        || PVFS_cast_block_to_file(vfs->block, block) != PVFS_OK) return PVFS_INVALID_LOCATION;
    return return_address;
}

std::int64_t PVFS_read_block_tree ( std::shared_ptr<PvfsFile> &vfs, int64_t address, std::shared_ptr<PvfsBlockTree> &block )
{
    int64_t return_address	=	0;

    if ( !vfs ) return PVFS_INVALID_LOCATION;
    if ( address == PVFS_INVALID_LOCATION ) return PVFS_INVALID_LOCATION;
    if ( !block  ) return PVFS_INVALID_LOCATION;

    return_address = PVFS_INVALID_LOCATION;

    return_address = PVFS_read_block ( vfs->fd, address, vfs->block );
    if (return_address != PVFS_BLOCK_HEADER_SIZE + vfs->blockSize
        || PVFS_cast_block_to_tree(vfs->block, block) != PVFS_OK) return PVFS_INVALID_LOCATION;
    return return_address;
}

std::int64_t PVFS_read_block_data ( std::shared_ptr<PvfsFile> &vfs, int64_t address, std::shared_ptr<PvfsBlockData> &block )
{
    int64_t return_address	=	0;

    if ( !vfs ) return PVFS_INVALID_LOCATION;
    if ( address == PVFS_INVALID_LOCATION ) return PVFS_INVALID_LOCATION;
    if ( !block ) return PVFS_INVALID_LOCATION;

    return_address = PVFS_INVALID_LOCATION;

    return_address = PVFS_read_block ( vfs->fd, address, vfs->block );
    if (return_address != PVFS_BLOCK_HEADER_SIZE + vfs->blockSize
        || PVFS_cast_block_to_data(vfs->block, block) != PVFS_OK) return PVFS_INVALID_LOCATION;
    return return_address;
}

std::int64_t PVFS_write_block_file ( std::shared_ptr<PvfsFile> &vfs, int64_t address, std::shared_ptr<PvfsBlockFile> &block )
{
    int64_t return_address	=	0;

    if ( !vfs ) return PVFS_INVALID_LOCATION;
    if ( address == PVFS_INVALID_LOCATION ) return PVFS_INVALID_LOCATION;
    if ( !block ) return PVFS_INVALID_LOCATION;

    return_address = PVFS_INVALID_LOCATION;
    if ( PVFS_cast_file_to_block ( block, vfs->block ) != PVFS_OK )
    {
        std::cerr << "Error: casting file to block" << std::endl;
         return PVFS_INVALID_LOCATION;
    }
    return_address = PVFS_write_block ( vfs->fd, address, vfs->block );

    return return_address;
}

std::int64_t PVFS_write_block_tree ( std::shared_ptr<PvfsFile> &vfs, int64_t address, std::shared_ptr<PvfsBlockTree> &block )
{
    int64_t return_address	=	0;

    if ( !vfs ) return PVFS_INVALID_LOCATION;
    if ( address == PVFS_INVALID_LOCATION ) return PVFS_INVALID_LOCATION;
    if ( !block ) return PVFS_INVALID_LOCATION;

    return_address = PVFS_INVALID_LOCATION;

    if ( PVFS_cast_tree_to_block ( block, vfs->block ) != PVFS_OK ) return PVFS_INVALID_LOCATION;
    return_address = PVFS_write_block ( vfs->fd, address, vfs->block );

    return return_address;
}

std::int64_t PVFS_write_block_data ( std::shared_ptr<PvfsFile> &vfs, int64_t address, std::shared_ptr<PvfsBlockData> &block )
{
    int64_t return_address;

    if ( !vfs ) return PVFS_INVALID_LOCATION;
    if ( address == PVFS_INVALID_LOCATION ) return PVFS_INVALID_LOCATION;
    if ( !block ) return PVFS_INVALID_LOCATION;

    return_address = PVFS_INVALID_LOCATION;

    if ( PVFS_cast_data_to_block ( block, vfs->block ) != PVFS_OK ) return PVFS_INVALID_LOCATION;
    return_address = PVFS_write_block ( vfs->fd, address, vfs->block );

    return return_address;
}


std::shared_ptr<PvfsFileHandle> create_PVFS_file_handle ( std::shared_ptr<PvfsFile> &vfs )
{
    if ( !vfs ) return nullptr;

    std::shared_ptr<PvfsFileHandle>	file = std::make_shared<PvfsFileHandle>();

    file->vfs				= vfs;  //store a shared pointer to the virtual file system.
    file->currentAddress	= -1; // 0
    file->dataAddress		= 0;
    memset ( file->info.filename, 0, PVFS_MAX_FILENAME_LENGTH );
    file->info.size			= 0;
    file->info.startBlock	= 0;
    file->error				= PVFS_OK;
    // A newly opened handle has nothing to write. In particular, seeking or
    // crossing a block while reading a readonly archive must not flush it.
    file->dirty = PVFS_CLEAN;

    file->block = create_PVFS_block ( vfs );
    if ( file->block == nullptr )
    {
        return nullptr;
    }
    file->data	= create_PVFS_block_data ( vfs );
    if ( file->data == nullptr )
    {
        return nullptr;
    }
    file->tree	= create_PVFS_block_tree ( vfs );
    if ( file->tree == nullptr )
    {
        return nullptr;
    }

    return file;
}


std::shared_ptr<PvfsFile> PVFS_create ( const char * filename )
{
    return PVFS_create_size ( filename, PVFS_DEFAULT_BLOCK_SIZE );
}

//If the file exists when this function is called, it will be erased
std::shared_ptr<PvfsFile> PVFS_create_size ( const char * filename, std::uint32_t block_size )
{
    std::shared_ptr<PvfsFile> vfs = create_PVFS_file_structure ( );
    if ( !vfs ) return nullptr;

    PVFS_file_set_blockSize ( vfs, block_size );
    vfs->fd = openRecordingWriter(filename, true);

    if(vfs->fd == PVFS_INVALID_FD)
    {
        std::cerr << "Error creating file" << std::endl;
        return nullptr;
    }

    PVFS_write_uint8 ( vfs->fd, 'P' );  //0
    PVFS_write_uint8 ( vfs->fd, 'V' );  //1
    PVFS_write_uint8 ( vfs->fd, 'F' );  //2
    PVFS_write_uint8 ( vfs->fd, 'S' );  //3
    PVFS_write_uint8 ( vfs->fd, vfs->version.major );  //4
    PVFS_write_uint8 ( vfs->fd, vfs->version.minor );  //5
    PVFS_write_uint16 ( vfs->fd, vfs->version.revision ); //6-7
    PVFS_write_sint32 ( vfs->fd, vfs->blockSize ); //8 - B  Byte order is reversed for all pvfs writes (little endian)
    PVFS_write_sint64 ( vfs->fd, vfs->tableLoc );  //C - 13

    // Initialize free block table (all entries empty)
    std::vector<FreeBlockTableEntry> freeBlockTable;
    freeBlockTable.resize(PVFS_FREE_BLOCK_TABLE_MAX_ENTRIES, FreeBlockTableEntry());
    PVFS_write_free_block_table ( vfs, freeBlockTable );

    // Zero out the rest of the header (if any space remains after free block table)
#ifdef _WIN32
    (void)_lseeki64 ( vfs->fd, vfs->tableLoc, SEEK_SET );
#else
    (void)lseek(vfs->fd, vfs->tableLoc, SEEK_SET);
#endif
    PVFS_write_uint8 ( vfs->fd, 0 );
    vfs->nextBlock = vfs->tableLoc;

    // Create the file table block.
    PVFS_allocate_block ( vfs );
    // Copy over the allocation information.
    PVFS_cast_block_to_file	( vfs->block, vfs->fileBlock );
    PVFS_write_block_file ( vfs, vfs->fileBlock->self, vfs->fileBlock );

    return vfs;
}


std::shared_ptr<PvfsFile> PVFS_open( const char * filename )
{
    char	file_id [ 4 ]	;

    std::shared_ptr<PvfsFile> vfs;
    vfs = create_PVFS_file_structure();
    if ( !vfs ) return nullptr;

    vfs->fd = openRecordingWriter(filename, false);
    if ( vfs->fd == PVFS_INVALID_FD )
    {
//        destroy_PVFS_file ( vfs );
        return nullptr;
    }

    // Check if this is the proper type of file.
    uint8_t tmp;
    PVFS_read_uint8 ( vfs->fd, tmp);
    file_id[ 0 ] = static_cast<char>(tmp);
    PVFS_read_uint8 ( vfs->fd, tmp);
    file_id[ 1 ] = static_cast<char>(tmp);
    PVFS_read_uint8 ( vfs->fd, tmp);
    file_id[ 2 ] = static_cast<char>(tmp);
    PVFS_read_uint8 ( vfs->fd, tmp);
    file_id[ 3 ] = static_cast<char>(tmp);
    if (( file_id [ 0 ] != 'P' ) ||
        ( file_id [ 1 ] != 'V' ) ||
        ( file_id [ 2 ] != 'F' ) ||
        ( file_id [ 3 ] != 'S' ))
    {
 //       destroy_PVFS_file ( vfs );
        return nullptr;
    }
    PVFS_read_uint8 ( vfs->fd, vfs->version.major );
    PVFS_read_uint8 ( vfs->fd, vfs->version.minor );
    PVFS_read_uint16 ( vfs->fd, vfs->version.revision );
    PVFS_read_sint32 ( vfs->fd, vfs->blockSize );
    PVFS_read_sint64 ( vfs->fd, vfs->tableLoc );

    PVFS_file_set_blockSize ( vfs, vfs->blockSize );

    // Read free block table (for version 3.1.0+, older versions will have zeros/random data)
    // This is safe - if version is old, table will just be empty
    std::vector<FreeBlockTableEntry> freeBlockTable;
    PVFS_read_free_block_table ( vfs, freeBlockTable ); // Ignore errors for backward compatibility

    // Find the end of the file and use that as where the next block goes.
    #ifdef _WIN32
    (void)_lseeki64 ( vfs->fd, 0, SEEK_END );
    vfs->nextBlock = _telli64 ( vfs->fd ) - 1;	// minus 1 for the eof marker...
    #else
    (void)lseek(vfs->fd, 0, SEEK_END);
    vfs->nextBlock = lseek(vfs->fd, 0, SEEK_CUR) - 1;
    #endif

    return vfs;
}

std::shared_ptr<PvfsFile> PVFS_open_readonly( const char * filename )
{
    char	file_id [ 4 ]	;

    std::shared_ptr<PvfsFile> vfs;
    vfs = create_PVFS_file_structure();
    if ( vfs == nullptr ) return nullptr;

#ifdef _WIN32
    vfs->fd = _open ( filename, _O_BINARY | _O_RDONLY, _S_IREAD );
#else
    vfs->fd = open(filename, O_RDONLY);
#endif
    if ( vfs->fd == PVFS_INVALID_FD )
    {
//        destroy_PVFS_file ( vfs );
        return nullptr;
    }

    // Check if this is the proper type of file.
    uint8_t tmp;
    PVFS_read_uint8 ( vfs->fd, tmp);
    file_id[ 0 ] = static_cast<char>(tmp);
    PVFS_read_uint8 ( vfs->fd, tmp);
    file_id[ 1 ] = static_cast<char>(tmp);
    PVFS_read_uint8 ( vfs->fd, tmp);
    file_id[ 2 ] = static_cast<char>(tmp);
    PVFS_read_uint8 ( vfs->fd, tmp);
    file_id[ 3 ] = static_cast<char>(tmp);
    if (( file_id [ 0 ] != 'P' ) ||
        ( file_id [ 1 ] != 'V' ) ||
        ( file_id [ 2 ] != 'F' ) ||
        ( file_id [ 3 ] != 'S' ))
    {
 //       destroy_PVFS_file ( vfs );
        return nullptr;
    }
    PVFS_read_uint8 ( vfs->fd, vfs->version.major );
    PVFS_read_uint8 ( vfs->fd, vfs->version.minor );
    PVFS_read_uint16 ( vfs->fd, vfs->version.revision );
    PVFS_read_sint32 ( vfs->fd, vfs->blockSize );
    PVFS_read_sint64 ( vfs->fd, vfs->tableLoc );

    PVFS_file_set_blockSize ( vfs, vfs->blockSize );

    // Read free block table (for version 3.1.0+, older versions will have zeros/random data)
    // This is safe - if version is old, table will just be empty
    std::vector<FreeBlockTableEntry> freeBlockTable;
    PVFS_read_free_block_table ( vfs, freeBlockTable ); // Ignore errors for backward compatibility

    // Find the end of the file and use that as where the next block goes.
    #ifdef _WIN32
    (void)_lseeki64 ( vfs->fd, 0, SEEK_END );
    vfs->nextBlock = _telli64 ( vfs->fd ) - 1;	// minus 1 for the eof marker...
    #else
    (void)lseek(vfs->fd, 0, SEEK_END);
    vfs->nextBlock = lseek(vfs->fd, 0, SEEK_CUR) - 1;
    #endif

    return vfs;
}

// Read the free block table from the header
std::int32_t PVFS_read_free_block_table(std::shared_ptr<PvfsFile> &vfs, std::vector<FreeBlockTableEntry> &table)
{
    if (!vfs) return PVFS_ARG_NULL;
    if (vfs->fd == PVFS_INVALID_FD) return PVFS_ERROR;

    table.clear();
    table.resize(PVFS_FREE_BLOCK_TABLE_MAX_ENTRIES, FreeBlockTableEntry());

    #ifdef _WIN32
    _lseeki64(vfs->fd, PVFS_HEADER_FREE_BLOCK_TABLE_DATA_OFFSET, SEEK_SET);
    #else
    lseek(vfs->fd, PVFS_HEADER_FREE_BLOCK_TABLE_DATA_OFFSET, SEEK_SET);
    #endif

    for (std::uint32_t i = 0; i < PVFS_FREE_BLOCK_TABLE_MAX_ENTRIES; i++)
    {
        PVFS_read_uint32(vfs->fd, table[i].blockSize);
        PVFS_read_sint64(vfs->fd, table[i].nextFreeBlock);
    }

    return PVFS_OK;
}

// Write the free block table to the header
std::int32_t PVFS_write_free_block_table(std::shared_ptr<PvfsFile> &vfs, const std::vector<FreeBlockTableEntry> &table)
{
    if (!vfs) return PVFS_ARG_NULL;
    if (vfs->fd == PVFS_INVALID_FD) return PVFS_ERROR;
    if (table.size() != PVFS_FREE_BLOCK_TABLE_MAX_ENTRIES) return PVFS_ERROR;

    if (!seekFile(vfs->fd, PVFS_HEADER_FREE_BLOCK_TABLE_DATA_OFFSET)) return PVFS_ERROR;
    for (const auto& entry : table) {
        if (!writeAll(vfs->fd, &entry.blockSize, sizeof(entry.blockSize))
            || !writeAll(vfs->fd, &entry.nextFreeBlock, sizeof(entry.nextFreeBlock))) return PVFS_ERROR;
    }

    return PVFS_OK;
}

// Add a free block to the appropriate chain in the free block table
std::int32_t PVFS_add_free_block(std::shared_ptr<PvfsFile> &vfs, std::int64_t blockAddress, std::uint32_t blockSize)
{
    if (!vfs) return PVFS_ARG_NULL;
    if (vfs->fd == PVFS_INVALID_FD) return PVFS_ERROR;
    if (blockAddress == PVFS_INVALID_LOCATION) return PVFS_ERROR;

    std::vector<FreeBlockTableEntry> table;
    if (PVFS_read_free_block_table(vfs, table) != PVFS_OK) return PVFS_ERROR;

    // Find entry with matching blockSize, or find empty entry
    std::int32_t entryIndex = -1;
    for (std::uint32_t i = 0; i < PVFS_FREE_BLOCK_TABLE_MAX_ENTRIES; i++)
    {
        if (table[i].blockSize == blockSize)
        {
            entryIndex = i;
            break;
        }
        if (entryIndex == -1 && table[i].blockSize == 0)
        {
            entryIndex = i; // Remember first empty slot
        }
    }

    if (entryIndex == -1) return PVFS_ERROR; // Table full

    // Read the block to get its current next pointer
    std::int64_t read_result = PVFS_read_block(vfs->fd, blockAddress, vfs->block);
    if (read_result == 0) return PVFS_ERROR;

    // Validate block state - if already FREE, this is a double delete or corruption
    // Abandon block reuse to prevent corruption
    if (vfs->block->type == PVFS_BLOCK_TYPE_FREE)
    {
        return PVFS_ERROR; // Block already marked as free - abandon reuse
    }

    // If this is a new entry, initialize it
    if (table[entryIndex].blockSize == 0)
    {
        table[entryIndex].blockSize = blockSize;
        table[entryIndex].nextFreeBlock = blockAddress;
        // Set the block's next to invalid (it's now the head of the chain)
        vfs->block->next = PVFS_INVALID_LOCATION;
    }
    else
    {
        // Add to front of chain
        vfs->block->next = table[entryIndex].nextFreeBlock;
        table[entryIndex].nextFreeBlock = blockAddress;
    }

    // Mark block as FREE and write it
    vfs->block->type = PVFS_BLOCK_TYPE_FREE;
    if (PVFS_write_block(vfs->fd, blockAddress, vfs->block) <= 0) return PVFS_ERROR;

    // Write updated table
    return PVFS_write_free_block_table(vfs, table);
}

// Get a free block from the appropriate chain in the free block table
std::int64_t PVFS_get_free_block(std::shared_ptr<PvfsFile> &vfs, std::uint32_t blockSize)
{
    if (!vfs) return PVFS_INVALID_LOCATION;
    if (vfs->fd == PVFS_INVALID_FD) return PVFS_INVALID_LOCATION;

    std::vector<FreeBlockTableEntry> table;
    if (PVFS_read_free_block_table(vfs, table) != PVFS_OK) return PVFS_INVALID_LOCATION;

    // Find entry with matching blockSize
    for (std::uint32_t i = 0; i < PVFS_FREE_BLOCK_TABLE_MAX_ENTRIES; i++)
    {
        if (table[i].blockSize == blockSize && table[i].nextFreeBlock != PVFS_INVALID_LOCATION)
        {
            std::int64_t freeBlock = table[i].nextFreeBlock;

            // Read the block to get its next pointer
            std::int64_t read_result = PVFS_read_block(vfs->fd, freeBlock, vfs->block);
            if (read_result == 0) return PVFS_INVALID_LOCATION;

            // Validate block state - if not FREE, free block table is corrupted
            // Abandon block reuse to prevent corruption
            if (vfs->block->type != PVFS_BLOCK_TYPE_FREE)
            {
                // Corrupt free block chain detected - clear this entry and abandon reuse
                table[i].blockSize = 0;
                table[i].nextFreeBlock = PVFS_INVALID_LOCATION;
                PVFS_write_free_block_table(vfs, table);
                return PVFS_INVALID_LOCATION;
            }

            // Update table entry to point to next block in chain
            table[i].nextFreeBlock = vfs->block->next;

            // If chain is now empty, clear the entry
            if (table[i].nextFreeBlock == PVFS_INVALID_LOCATION)
            {
                table[i].blockSize = 0;
            }

            // Write updated table
            if (PVFS_write_free_block_table(vfs, table) != PVFS_OK) return PVFS_INVALID_LOCATION;

            return freeBlock;
        }
    }

    return PVFS_INVALID_LOCATION; // No free block of this size
}

// Forces a disk allocation to accomodate the next data block
// When PVFS_ENABLE_BLOCK_REUSE is 1 (default), checks free block table first; else always appends at end.
std::int64_t PVFS_allocate_block(std::shared_ptr<PvfsFile> &pvfs)
{

    // Error check
    if (!pvfs) return 0;

    // Try to get a free block from the free block table
    std::int64_t freeBlock = PVFS_get_free_block(pvfs, pvfs->blockSize);
    if (freeBlock != PVFS_INVALID_LOCATION)
    {
        // Found a free block, use it
        clearBlock<PvfsBlock>(pvfs->block, pvfs->blockSize);
        pvfs->block->self = freeBlock;
        return freeBlock;
    }

    // No free block available (or block reuse disabled), allocate new block at end of file
    clearBlock<PvfsBlock>(pvfs->block, pvfs->block->size);

    const auto address = pvfs->nextBlock;
    const auto end = address + pvfs->blockSize + PVFS_BLOCK_HEADER_SIZE;
    const uint8_t marker = 0xff;
    if (!seekFile(pvfs->fd, end) || !writeAll(pvfs->fd, &marker, 1)) return 0;
    pvfs->block->self = address;
    pvfs->nextBlock = end;
    return address;
}



std::shared_ptr<PvfsFileHandle> PVFS_fcreate ( std::shared_ptr<PvfsFile> &vfs, const char * filename )
{
    std::shared_ptr<PvfsFileHandle>	file = std::make_shared<PvfsFileHandle>();			// File handle to create.
    std::shared_ptr<PvfsBlockFile>	file_block;		// File block we are editing.
    pvfs::PvfsFileEntry             file_entry;		// Pointer to a file blocks entry.
    // Error Check
    if ( !vfs ) return nullptr;
    if ( filename == nullptr ) return nullptr;
    if ( vfs->fd == PVFS_INVALID_FD ) return nullptr;

    // First, look for a deleted (zeroed) file entry slot to reuse. This prevents the file block
    // chain from growing unbounded when the same filename is repeatedly overwritten.
    std::int32_t reuseIndex = -1;
    std::int64_t address = PVFS_read_block_file ( vfs, vfs->tableLoc, vfs->fileBlock );
    while ( address != PVFS_INVALID_LOCATION )
    {
        for ( std::uint32_t i = 0; i < static_cast<std::uint32_t>(vfs->fileBlock->files.size()); i++ )
        {
            PvfsFileEntry* entry = &(vfs->fileBlock->files [ i ]);
            if ( entry->filename [ 0 ] == 0 )
            {
                // Deleted slot (PVFS_delete_file zeros filename, startBlock, size)
                file_block = vfs->fileBlock;
                reuseIndex = static_cast<std::int32_t>(i);
                address = PVFS_INVALID_LOCATION;  // break outer loop
                break;
            }
        }
        if ( address != PVFS_INVALID_LOCATION )
        {
            address = PVFS_read_block_file ( vfs, vfs->fileBlock->next, vfs->fileBlock );
        }
    }

    if ( reuseIndex < 0 )
    {
        // No deleted slot found: use last block and append (possibly allocate new file block).
        PVFS_read_block_file ( vfs, vfs->tableLoc, vfs->fileBlock );
        file_block = vfs->fileBlock;

        while ( file_block->next != PVFS_INVALID_LOCATION )
        {
            PVFS_read_block_file ( vfs, file_block->next, vfs->fileBlock );
            file_block = vfs->fileBlock;		// Not really neccessary...
        }
        // Check the final block is full or not.
        if ( file_block->files.size() == vfs->fileMaxCount )
        {
            // Allocate a new block for file storage.
            PVFS_allocate_block ( vfs );
            PVFS_cast_block_to_file ( vfs->block, vfs->fileBlockTemp );
            file_block->next = vfs->fileBlockTemp->self;
            vfs->fileBlockTemp->prev = file_block->self;
            PVFS_write_block_file ( vfs, file_block->self, file_block );			// Save back the change
            PVFS_write_block_file ( vfs, vfs->fileBlockTemp->self, vfs->fileBlockTemp );

            // Reload the block, reset the pointer.
            PVFS_read_block_file ( vfs, vfs->fileBlockTemp->self, vfs->fileBlock );
            file_block = vfs->fileBlock;

        }
    }

    file = create_PVFS_file_handle ( vfs );
    if ( !file )
    {
        return nullptr;
    }

    // Create the (current) tree block.
    PVFS_allocate_block ( vfs );
    PVFS_cast_block_to_tree ( vfs->block, file->tree );

    // Create the starting data block.
    PVFS_allocate_block ( vfs );
    PVFS_cast_block_to_data ( vfs->block, file->data );
    file->data->tree = file->tree->self;
    PVFS_write_block_data ( vfs, file->data->self, file->data );	// Clear the block.

    // Add the data block to the tree, and write out the tree block.
    file->tree->mappings [ 0 ].address = 0;
    file->tree->mappings [ 0 ].blockLoc = file->data->self;
    file->tree->count++;
    file->tree->up = PVFS_INVALID_LOCATION;
    PVFS_write_block_tree ( vfs, file->tree->self, file->tree );

    // Create the entry, start at the tree.
    strcpy ( (char*) file_entry.filename, filename );
    file_entry.size		= 0;
    file_entry.startBlock = file->tree->self;

    if ( reuseIndex >= 0 )
    {
        // Reuse the deleted slot; do not grow the file block list.
        file_block->files [ static_cast<std::size_t>(reuseIndex) ] = file_entry;
        file->tableBlock = file_block->self;
        file->tableIndex = reuseIndex;
    }
    else
    {
        file_block->files.push_back(file_entry);
        file->tableBlock		= file_block->self;
        file->tableIndex		= static_cast<std::int32_t>(file_block->files.size() - 1);  // Index of the entry we just added
    }

    // Get the file information updated.
    PVFS_copy_fileEntry  ( &(file->info), &file_entry );

    // Save the block (count is now derived from files.size() when writing)
    PVFS_write_block_file ( vfs, file_block->self, file_block );

    // Seek to the start position
    PVFS_seek ( file, 0 );

    //PVFS_unlock(vfs);

    return file;
}



std::int32_t PVFS_get_channel_list(std::shared_ptr<PvfsFile> &vfs, std::vector<std::string> &names)
{
    std::uint32_t					i			=	0;
    PvfsFileEntry *		            fileEntry	=	nullptr;
    std::int64_t					address		=	0;			// Current address of the block

    // Error Check
    if ( !vfs ) return PVFS_ARG_NULL;
    if ( vfs->fd == PVFS_INVALID_FD ) return PVFS_ARG_NULL;

    // Get the first file block.
    address = PVFS_read_block_file ( vfs, vfs->tableLoc, vfs->fileBlock );
    while ( address != PVFS_INVALID_LOCATION )
    {
        for ( i = 0; i < static_cast<std::uint32_t>(vfs->fileBlock->files.size()); i++ )
        {
            // Point to the entry.
            fileEntry = &(vfs->fileBlock->files [ i ]);
            if(fileEntry->filename[0] != 0) //If there is a named file
            {
                names.push_back(std::string(reinterpret_cast<char*>(fileEntry->filename)));
            }
        }
        // Get the next location.
        address = PVFS_read_block_file ( vfs, vfs->fileBlock->next, vfs->fileBlock );
    }
    return PVFS_OK;
}


std::shared_ptr<PvfsFileHandle> PVFS_fopen ( std::shared_ptr<PvfsFile> &vfs, const char * filename )
{
    std::uint32_t					i			=	0;			// Iterator through the file entries.
    std::shared_ptr<PvfsFileHandle>	file;	                    // File handle to create.
    PvfsFileEntry *		            fileEntry	=	nullptr;
    std::int64_t					address		=	0;			// Current address of the block

    // Error Check
    if ( !vfs ) return nullptr;
    if ( filename == nullptr ) return nullptr;
    if ( vfs->fd == PVFS_INVALID_FD ) return nullptr;
    // Get the first file block.
    address = PVFS_read_block_file ( vfs, vfs->tableLoc, vfs->fileBlock );
    while ( address != PVFS_INVALID_LOCATION )
    {
        for ( i = 0; i < static_cast<std::uint32_t>(vfs->fileBlock->files.size()); i++ )
        {
            // Point to the entry.
            fileEntry = &(vfs->fileBlock->files [ i ]);

            // If the names match, create a file handle and return.
            if ( strcmp ( (char*) fileEntry->filename, filename ) == 0 )
            {
                file = create_PVFS_file_handle ( vfs );
                if ( file == nullptr )
                {
                    return nullptr;
                }
                PVFS_copy_fileEntry ( &(file->info), fileEntry );
                file->tableBlock		= vfs->fileBlock->self;
                file->tableIndex		= i;    //vfs->fileBlock->count;
                PVFS_seek ( file, 0 );			// Reset the file to 0 location.
                return file;
            }
        }
        // Get the next location.
        address = PVFS_read_block_file ( vfs, vfs->fileBlock->next, vfs->fileBlock );
    }

    return nullptr;
}



// Helper function with visited set to prevent infinite loops
static std::int32_t PVFS_collect_file_blocks_impl(std::shared_ptr<PvfsFile> &vfs, std::int64_t startBlock,
                                                    std::vector<std::int64_t> &treeBlocks,
                                                    std::vector<std::int64_t> &dataBlocks,
                                                    std::set<std::int64_t> &visited)
{
    if ( !vfs ) return PVFS_ARG_NULL;
    if ( startBlock == PVFS_INVALID_LOCATION ) return PVFS_OK;

    // Check if we've already visited this block (cycle detection)
    if ( visited.find(startBlock) != visited.end() )
    {
        // Already visited - skip to prevent infinite loop
        return PVFS_OK;
    }
    visited.insert(startBlock);

    std::shared_ptr<PvfsBlock> block = vfs->block;
    std::shared_ptr<PvfsBlockTree> tree = vfs->treeBlockTemp;
    std::shared_ptr<PvfsBlockData> data = vfs->dataBlockTemp;

    // Read the block at startBlock
    std::int64_t read_result = PVFS_read_block ( vfs->fd, startBlock, block );
    if ( read_result == 0 ) return PVFS_ERROR;

    if ( block->type == PVFS_BLOCK_TYPE_TREE )
    {
        // Cast to tree block
        if ( PVFS_cast_block_to_tree ( block, tree ) != PVFS_OK ) return PVFS_ERROR;

        // Add this tree block to the list
        treeBlocks.push_back ( startBlock );

        // Recursively process all mappings in this tree block
        for ( std::int32_t i = 0; i < tree->count; i++ )
        {
            std::int64_t mappedBlock = tree->mappings [ i ].blockLoc;
            if ( mappedBlock != PVFS_INVALID_LOCATION )
            {
                // Recursively collect blocks from the mapped location
                PVFS_collect_file_blocks_impl ( vfs, mappedBlock, treeBlocks, dataBlocks, visited );
            }
        }
    }
    else if ( block->type == PVFS_BLOCK_TYPE_DATA )
    {
        // Cast to data block
        if ( PVFS_cast_block_to_data ( block, data ) != PVFS_OK ) return PVFS_ERROR;

        // Add this data block to the list
        dataBlocks.push_back ( startBlock );

        // Follow the linked list of data blocks
        if ( data->next != PVFS_INVALID_LOCATION )
        {
            PVFS_collect_file_blocks_impl ( vfs, data->next, treeBlocks, dataBlocks, visited );
        }
    }

    return PVFS_OK;
}

// Helper function to recursively collect all blocks (tree and data) for a file
std::int32_t PVFS_collect_file_blocks(std::shared_ptr<PvfsFile> &vfs, std::int64_t startBlock, std::vector<std::int64_t> &treeBlocks, std::vector<std::int64_t> &dataBlocks)
{
    std::set<std::int64_t> visited;
    return PVFS_collect_file_blocks_impl(vfs, startBlock, treeBlocks, dataBlocks, visited);
}

std::int32_t PVFS_delete_file(std::shared_ptr<PvfsFile> &vfs, const char* filename)
{
    // Error Check
    if ( !vfs ) return pvfs::PVFS_ARG_NULL;
    if ( filename == nullptr ) return pvfs::PVFS_ARG_NULL;
    if ( vfs->fd == PVFS_INVALID_FD ) return pvfs::PVFS_ERROR;

    // Find the file entry in all file blocks
    std::int64_t address = PVFS_read_block_file ( vfs, vfs->tableLoc, vfs->fileBlock );
    std::uint32_t i = 0;
    pvfs::PvfsFileEntry * fileEntry = nullptr;
    std::int64_t tableBlock = PVFS_INVALID_LOCATION;
    std::int32_t tableIndex = -1;
    std::int64_t fileStartBlock = PVFS_INVALID_LOCATION;

    bool found = false;

    // Search through all file blocks
    while ( address != PVFS_INVALID_LOCATION && !found )
    {
        for ( i = 0; i < static_cast<std::uint32_t>(vfs->fileBlock->files.size()); i++ )
        {
            fileEntry = &(vfs->fileBlock->files [ i ]);
            if ( fileEntry != nullptr && strcmp ( (char*) fileEntry->filename, filename ) == 0 )
            {
                // Found the file - save its information
                tableBlock = vfs->fileBlock->self;
                tableIndex = i;
                fileStartBlock = fileEntry->startBlock;
                found = true;
                break;
            }
        }

        if ( !found )
        {
            // Get the next file block
            address = PVFS_read_block_file ( vfs, vfs->fileBlock->next, vfs->fileBlock );
        }
    }

    if ( !found ) return pvfs::PVFS_ERROR;

    // Collect all blocks associated with this file
    std::vector<std::int64_t> treeBlocks;
    std::vector<std::int64_t> dataBlocks;

    if ( PVFS_collect_file_blocks ( vfs, fileStartBlock, treeBlocks, dataBlocks ) != PVFS_OK )
    {
        return pvfs::PVFS_ERROR;
    }

    // Add all data blocks to free block table
    for ( std::int64_t blockAddr : dataBlocks )
    {
        PVFS_add_free_block ( vfs, blockAddr, vfs->blockSize );
    }

    // Add all tree blocks to free block table
    for ( std::int64_t blockAddr : treeBlocks )
    {
        PVFS_add_free_block ( vfs, blockAddr, vfs->blockSize );
    }

    // Remove the file entry from the file table
    // Reload the file block where the entry is located
    if ( PVFS_read_block_file ( vfs, tableBlock, vfs->fileBlock ) == PVFS_INVALID_LOCATION )
    {
        return pvfs::PVFS_ERROR;
    }

    // Zero out the filename to mark as deleted
    if ( tableIndex >= 0 && tableIndex < static_cast<std::int32_t>(vfs->fileBlock->files.size()) )
    {
        memset ( vfs->fileBlock->files [ tableIndex ].filename, 0, PVFS_MAX_FILENAME_LENGTH );
        vfs->fileBlock->files [ tableIndex ].startBlock = 0;
        vfs->fileBlock->files [ tableIndex ].size = 0;
        PVFS_write_block_file ( vfs, tableBlock, vfs->fileBlock );
    }

    return pvfs::PVFS_OK;
}


bool PVFS_has_file ( std::shared_ptr<PvfsFile> vfs, const char * filename )
{
    std::uint32_t		i				=	0;				// Iterator through the file entries.
    PvfsFileEntry *		fileEntry		=	nullptr;
    std::int64_t		address			=	0;				// Current address of the block

    // Error Check
    if ( !vfs ) return false;
    if ( filename == nullptr ) return false;
    if ( vfs->fd == PVFS_INVALID_FD ) return false;

    // Get the first file block.
    address = PVFS_read_block_file ( vfs, vfs->tableLoc, vfs->fileBlock );

    while ( address != PVFS_INVALID_LOCATION )
    {
        for ( i = 0; i < static_cast<std::uint32_t>(vfs->fileBlock->files.size()); i++ )
        {
            // Point to the entry.
            fileEntry = &(vfs->fileBlock->files [ i ]);
            // If the names match we found it create and return.
            if ( strcmp ( (char*) fileEntry->filename, filename ) == 0 )
            {
                return true;
            }
        }
        // Get the next location.
        address = PVFS_read_block_file ( vfs, vfs->fileBlock->next, vfs->fileBlock );
    }

    return false;
}


/*!*****************************************************
*	PROCEDURE NAME:
*		PVFS_get_file_list
*
*	DESCRIPTION:
*		Finds the list of non-zero sized files in the PVFS.
*
*	\param vfs The vfs to search
*	\param filenames	An array of size vfs->fileMaxCount to store
*						the list of filenames of size PVFS_MAX_FILENAME_LENGTH.
*						On success, the array will contain the list of files
*						in the VFS.
*	\return	The number of files found and names stored in the filenames array
*
*	\brief
*		Finds the list of non-zero sized files in the PVFS.
*
*//******************************************************/

std::uint32_t 	PVFS_get_file_list ( std::shared_ptr<pvfs::PvfsFile> &vfs, std::vector<std::string> &filenames)
{
    std::uint32_t				                i		    =	0;				// Iterator throug the file entries.
    std::shared_ptr<pvfs::PvfsFileHandle>	    file		=	nullptr;		// File handle to create.
    pvfs::PvfsFileEntry *		                fileEntry	=	nullptr;
    std::int64_t				                address		=	0;				// Current address of the block
    std::uint32_t				                num_files	=	0;				// Count the number of files found

    // Error Check
    if ( !vfs ) return PVFS_ARG_NULL;
    if ( vfs->fd == PVFS_INVALID_FD ) return PVFS_ARG_NULL;

    filenames.clear();

    // Get the first file block.
    address = PVFS_read_block_file ( vfs, vfs->tableLoc, vfs->fileBlock );
    while ( address != PVFS_INVALID_LOCATION )
    {
        for ( i = 0; i < static_cast<std::uint32_t>(vfs->fileBlock->files.size()); i++ )
        {
            // Point to the entry.
            fileEntry = &(vfs->fileBlock->files [ i ]);
            if ( strlen ( (char*)fileEntry->filename ) <= 0 )
            {
                continue;
            }

            // Attempt to open the file before adding it to the list
            file = PVFS_fopen ( vfs, (char*)fileEntry->filename );
            if ( !file )
            {
                continue;
            }

            // Add the file to the list and close the file
           std::string filenameString(reinterpret_cast<char*>(fileEntry->filename));
            filenames.push_back( filenameString );
            num_files++;
            PVFS_fclose ( file );
        }
        // Get the next location.
        address = PVFS_read_block_file ( vfs, vfs->fileBlock->next, vfs->fileBlock );
    }

    //PVFS_unlock ( vfs );
    return PVFS_OK;
}

// Where are we in the virtual file.
std::int64_t PVFS_tell ( std::shared_ptr<PvfsFileHandle> &vf )
{
    if ( !vf ) return PVFS_INVALID_LOCATION;

    return vf->currentAddress;
}


// Seek to some location in the virtual file.
std::int64_t PVFS_seek ( std::shared_ptr<PvfsFileHandle> &vf, int64_t address )
{
    std::int64_t			cur_address	=	0;		            // Current address to read.
    std::int32_t			i			=	0;					// For interating through the tree map.
    std::shared_ptr<PvfsBlockTree>	    tree;
    std::shared_ptr<PvfsBlock>          block;
    int						fd			=	0;
    PvfsLocationMap *	    map			=	nullptr;
    std::int8_t				found		=	0;
    std::int64_t			read		=	0;

    if ( !vf ) return PVFS_INVALID_LOCATION;
    if ( address > vf->info.size ) return PVFS_INVALID_LOCATION;

    // Check to see if we are already at the address (i.e. no need to move.).
    if ( vf->currentAddress == address ) return PVFS_OK;

    // Prevent the current block from not getting written.
    PVFS_flush ( vf );

    // Extract pointers.
    cur_address		= vf->info.startBlock;
    block           = vf->vfs->block;
    tree            = vf->tree;
    fd				= vf->vfs->fd;
    found			= 0;
    map				= nullptr;

    read = PVFS_read_block ( fd, cur_address, block );

    if ( read == 0 ) return PVFS_INVALID_LOCATION;

    while ( block->type == PVFS_BLOCK_TYPE_TREE )
    {


        // Check if there are entries to look through.
        // Only 1 entry it *has* to be down that path.
        if(PVFS_cast_block_to_tree ( block, tree ) == PVFS_OK){
            if ( tree->count > 1 )
            {
                // Start at 1 so we can back up one entry in the tree.
                found = 0;
                for ( i = 1; i < tree->count; i++ )
                {
                    map = &(tree->mappings [ i ]);
                    // Went too far back up one.
                    if ( map->address > address )
                    {
                        map = &(tree->mappings [ i - 1 ]);
                        cur_address = map->blockLoc;
                        i = tree->count;
                        found = 1;
                    }
                }
                if ( found == 0 )
                {
                    map = &(tree->mappings [ i - 1 ]);
                    cur_address = map->blockLoc;
                }
            }
            else
            {
                map = &(tree->mappings [ 0 ]);
                cur_address = map->blockLoc;
            }

        }else{//tree is nullptr
            return PVFS_INVALID_LOCATION;
        }

        read = PVFS_read_block ( fd, cur_address, block );
        if ( read == 0 )	// we have a problem.
        {
            return PVFS_INVALID_LOCATION;
        }

        if ( block->type == PVFS_BLOCK_TYPE_DATA )
        {
            found = 1;
        }
    }

    if ( block->type != PVFS_BLOCK_TYPE_DATA ) return PVFS_INVALID_LOCATION;
    if ( !found ) return PVFS_INVALID_LOCATION;
    if ( map == nullptr ) return PVFS_INVALID_LOCATION;	// Somehow.

    PVFS_cast_block_to_data ( block, vf->data );
    vf->currentAddress = address;
    vf->dataAddress	= (int32_t) (address - map->address);

    //return address;
    return PVFS_OK;
}


std::int32_t PVFS_write ( std::shared_ptr<PvfsFileHandle> &vf, const uint8_t * buffer, uint32_t size )
{
    std::uint32_t				i				=	0;
    std::int32_t				count			=	0;
    std::int64_t				address			=	0;			// temp for saving the address of the current data block.
    std::int64_t				tree_address	=	0;			// Address for keeping track of the tree of the current data block.
    std::int32_t				copy_rem		=	0;			// Remaing for copy
    std::int32_t				block_rem		=	0;			// Remaining in the block

    if ( !vf ) return PVFS_ARG_NULL;
    if ( buffer == nullptr ) return PVFS_ARG_NULL;
    if ( size <= 0 ) return 0;
    if (!vf->vfs || !vf->data || vf->error == PVFS_ERROR) return PVFS_ERROR;

    // Set the dirt bit
    vf->dirty	= PVFS_DIRTY;		// From the putc

    count = 0;
    i = 0;
    while ( i < size )
    {
        vf->dirty = PVFS_DIRTY;
        copy_rem = size - i;
        block_rem = vf->data->maxCount - vf->dataAddress;
        if ( copy_rem < block_rem )
        {
/*            for(std::int32_t j = 0; j < copy_rem; j++)
            {
                vf->data->data[vf->dataAddress+ j] = *(buffer + i + j);
            } */

            memcpy ( vf->data->data.data() + vf->dataAddress, buffer + i, copy_rem );

            i += copy_rem;
            count += copy_rem;
            vf->dataAddress += copy_rem;
            vf->currentAddress += copy_rem;

            // We are adding to the file, which means there will be an allocation in the future.
            if ( vf->currentAddress >= vf->info.size )
            {
                vf->info.size = vf->currentAddress;
            }

            if ( vf->dataAddress > vf->data->count )
            {
                vf->data->count = vf->dataAddress;
            }
        }
        else
        {
/*            for(std::int32_t j = 0; j < block_rem; j++)
            {
                vf->data->data[vf->dataAddress + j] = *(buffer + i + j);
            } */
            memcpy ( vf->data->data.data() + vf->dataAddress, buffer + i, block_rem );


            i += block_rem;
            count += block_rem;
            vf->currentAddress += block_rem;

            // If we are adding to the file, there will be an allocation in the future.
            if ( vf->currentAddress >= vf->info.size )
            {
                vf->info.size = vf->currentAddress;
            }

            vf->data->count = vf->data->maxCount;
            if (PVFS_flush(vf) != PVFS_OK) { vf->error = PVFS_ERROR; return PVFS_ERROR; }
            vf->dirty = PVFS_DIRTY;
            vf->dataAddress = 0;

            if ( vf->data->next == PVFS_INVALID_LOCATION )
            {
                // Create a new data block.
                if (PVFS_allocate_block(vf->vfs) <= 0) { vf->error = PVFS_ERROR; return PVFS_ERROR; }

                if(vf->vfs == nullptr)		return -1;
                PVFS_cast_block_to_data ( vf->vfs->block, vf->vfs->dataBlockTemp );

                if(vf->vfs->dataBlockTemp == nullptr)	return -1;

                // Keep the double link going.
                vf->data->next					= vf->vfs->dataBlockTemp->self;
                tree_address					= vf->data->tree;
                vf->vfs->dataBlockTemp->prev	= vf->data->self;
                if (PVFS_write_block_data ( vf->vfs, vf->data->self, vf->data ) <= 0) { vf->error = PVFS_ERROR; return PVFS_ERROR; }

                // Need a not write-read way to do this...
                address = vf->vfs->dataBlockTemp->self;
                if (PVFS_write_block_data ( vf->vfs, address, vf->vfs->dataBlockTemp ) <= 0) { vf->error = PVFS_ERROR; return PVFS_ERROR; }
                if (PVFS_read_block_data ( vf->vfs, address, vf->data ) == PVFS_INVALID_LOCATION) { vf->error = PVFS_ERROR; return PVFS_ERROR; }

                /////
                // Add to the tree....
                if (PVFS_read_block_tree ( vf->vfs, tree_address, vf->tree ) == PVFS_INVALID_LOCATION) { vf->error = PVFS_ERROR; return PVFS_ERROR; }
                std::shared_ptr<PvfsLocationMap>	map = std::make_shared<PvfsLocationMap>();
                map->address	= vf->currentAddress;
                map->blockLoc	= vf->data->self;
                if (PVFS_tree_add_data(vf, vf->tree, map, vf->data) != PVFS_OK) { vf->error = PVFS_ERROR; return PVFS_ERROR; }
            }
            else
            {
                if (PVFS_read_block_data ( vf->vfs, vf->data->next, vf->data ) == PVFS_INVALID_LOCATION) { vf->error = PVFS_ERROR; return PVFS_ERROR; }
            }
        }
    }
    return count;
}

std::int32_t PVFS_read ( std::shared_ptr<PvfsFileHandle> &vf, uint8_t * buffer, uint32_t size )
{
    std::uint32_t	i			=	0;
    std::int32_t	copy_rem	=	0;	// Remaing for copy
    std::int32_t	block_rem	=	0;	// Remaining in the block
    std::int64_t	file_rem	=	0;	// Remaining in the file
    std::int32_t	count		=	0;

    // Error checking.
    if ( !vf ) return PVFS_ARG_NULL;
    if ( buffer == nullptr ) return PVFS_ARG_NULL;
    if ( size <= 0 ) return 0;;
    if ( vf->currentAddress >= vf->info.size )
    {
        vf->error = PVFS_EOF;
        return 0;
    }
    // Adjust size if it greater than that remaining in the file.
    file_rem = vf->info.size - vf->currentAddress;
    if ( size > file_rem )
    {
        size = file_rem;
    }
    count = 0;
    i = 0;
    while( i < size )
    {
        // If the remainder fits use it.
        copy_rem = size - i;
        block_rem = vf->data->maxCount - vf->dataAddress;
        if ( copy_rem < block_rem )
        {
            for(std::int32_t j = 0; j < copy_rem; j++)
            {
                *(buffer + i + j) = vf->data->data[vf->dataAddress + j];
            }
//            memcpy ( buffer + i, vf->data->data + vf->dataAddress, copy_rem );

            i += copy_rem;
            count += copy_rem;
            vf->dataAddress += copy_rem;
            vf->currentAddress += copy_rem;
        }
        // Massive Error, should not be negative.
        else if ( block_rem <= 0 )
        {
            // Return what we've read so far.
            return count;
        }
        else
        {
            for(std::int32_t j = 0; j < block_rem; j++)
            {
                *(buffer + i + j) = vf->data->data[vf->dataAddress + j];

            }
//            memcpy ( buffer + i, vf->data->data + vf->dataAddress, block_rem );
            if (PVFS_flush(vf) != PVFS_OK) return PVFS_ERROR;
            if (vf->currentAddress + block_rem < vf->info.size
                && PVFS_read_block_data(vf->vfs, vf->data->next, vf->data) == PVFS_INVALID_LOCATION)
                return PVFS_ERROR;
            vf->dataAddress = 0;

            i += block_rem;
            count += block_rem;
            vf->currentAddress += block_rem;
        }
    }

    return count;
}


std::int32_t PVFS_fclose ( std::shared_ptr<PvfsFileHandle> &vf )
{
    if ( !vf ) return PVFS_OK;
    const auto result = PVFS_flush(vf);
    vf = nullptr;
    return result;
}

//PVFS_dirty is only set by PVFS_Write
//
//The read/write behavior is necessary here because the buffers in vfs
//and vf are being reused.  That is, the only source of truth is
//the disk
//
//Read the current file block
//Create a pointer to the file entry at vf->tableIndex
//Copy the updated file entry vf->info to the pointer location
//Write the data back into fileBlock
//
//Write out the current data and tree blocks
//
// commit to disk.  The commit step is not strictly necessary since the OS
// will handle this.  It may be faster to omit a commit at this point
// and rely on the OS or handle it elsewhere.  That is the way the original c code was structured.
//
// Adding a commit flag to this function that defaults to false to mimic the
// original behavior.
//

std::int32_t PVFS_flush(std::shared_ptr<PvfsFileHandle>& vf, bool commit)
{
    if (!vf || !vf->vfs) return PVFS_ARG_NULL;
    if (vf->error == PVFS_ERROR) return PVFS_ERROR;
    if (vf->dirty != PVFS_CLEAN) {
        if (!vf->data || !vf->tree
            || PVFS_write_block_data(vf->vfs, vf->data->self, vf->data) <= 0
            || PVFS_write_block_tree(vf->vfs, vf->tree->self, vf->tree) <= 0) return PVFS_ERROR;
        if (vf->tableIndex >= 0) {
            if (PVFS_read_block_file(vf->vfs, vf->tableBlock, vf->vfs->fileBlock) == PVFS_INVALID_LOCATION
                || static_cast<size_t>(vf->tableIndex) >= vf->vfs->fileBlock->files.size()) return PVFS_ERROR;
            vf->vfs->fileBlock->files[vf->tableIndex] = vf->info;
            if (PVFS_write_block_file(vf->vfs, vf->tableBlock, vf->vfs->fileBlock) <= 0) return PVFS_ERROR;
        }
    }
    if (commit && !commitFile(vf->vfs->fd)) return PVFS_ERROR;
    vf->dirty = PVFS_CLEAN;
    return PVFS_OK;
}

std::int32_t PVFS_tree_add_data ( std::shared_ptr<PvfsFileHandle> &vf, std::shared_ptr<PvfsBlockTree> &tree, std::shared_ptr<PvfsLocationMap> &map, std::shared_ptr<PvfsBlockData> &data )
{
    std::int64_t					address			=	0;		// Tree address up from the one passed in.
    PvfsFileEntry*		            fileEntry;	                // Pointer to the file entry.
    std::shared_ptr<PvfsBlockTree>		new_root;	// New root of the tree.

    if ( !vf ) return PVFS_ARG_NULL;
    if ( !tree ) return PVFS_ARG_NULL;
    if ( !map ) return PVFS_ARG_NULL;

    // If there is room in the tree branch just above add to it.
    if ( tree->count < tree->maxMappings )
    {
        tree->mappings [ tree->count ].address		= map->address;
        tree->mappings [ tree->count ].blockLoc	= map->blockLoc;
        tree->count++;
        if (PVFS_write_block_tree ( vf->vfs, tree->self, tree ) <= 0) return PVFS_ERROR;
        data->tree = tree->self;
        if (PVFS_write_block_data ( vf->vfs, data->self, data ) <= 0) return PVFS_ERROR;
        return PVFS_OK;
    }
    else // We have to mess with the tree.
    {
        // Allocate a new tree branch.
        // Add the map to that branch.
        // Add that branch to the tree up.
        // If the tree up from the branch is root
        //		create a new root with the old root as the first map.
        //      then add the new branch.
        address = tree->up;

        if (PVFS_allocate_block(vf->vfs) <= 0) return PVFS_ERROR;
        if(vf->vfs != nullptr){
            PVFS_cast_block_to_tree ( vf->vfs->block, vf->vfs->treeBlockTemp );
            vf->vfs->treeBlockTemp->up = address;					// Set so the up from this branch is the same as the other.
            if (PVFS_tree_add ( vf, vf->vfs->treeBlockTemp, map ) != PVFS_OK) return PVFS_ERROR;	// Should return quickly from the top part.
        }else{
            return PVFS_ARG_NULL;
        }


        // Set the data to point to the branch.
        //data->tree = address;
        data->tree = vf->vfs->treeBlockTemp->self;
        if (PVFS_write_block_data ( vf->vfs, data->self, data ) <= 0) return PVFS_ERROR;

        map->address	= vf->currentAddress;
        map->blockLoc	= vf->vfs->treeBlockTemp->self;

        if ( address != PVFS_INVALID_LOCATION )
        {
            if (PVFS_read_block_tree ( vf->vfs, address, vf->tree ) == PVFS_INVALID_LOCATION) return PVFS_ERROR;
            return PVFS_tree_add ( vf, vf->tree, map );
        }
        else
        {
            // Create a new root.
            new_root = create_PVFS_block_tree ( vf->vfs );
            if (PVFS_allocate_block(vf->vfs) <= 0) return PVFS_ERROR;
            if(vf->vfs != nullptr){
                PVFS_cast_block_to_tree ( vf->vfs->block, new_root );
            }
            new_root->up = PVFS_INVALID_LOCATION;

            // Old tree has the new root.
            tree->up = new_root->self;
            if (PVFS_write_block_tree ( vf->vfs, tree->self, tree ) <= 0) return PVFS_ERROR;

            // New tree gets the old map.
            map->address	= tree->mappings [ 0 ].address;
            map->blockLoc	= tree->self;
            if (PVFS_tree_add ( vf, new_root, map ) != PVFS_OK) return PVFS_ERROR;		// Should return quickly.

            // Alter the tree passed in to have the new tree as the upper.
            map->address	= vf->currentAddress;
            if(vf->vfs){
                if(vf->vfs->treeBlockTemp){
                    map->blockLoc	= vf->vfs->treeBlockTemp->self;
                    if (PVFS_tree_add ( vf, new_root, map ) != PVFS_OK) return PVFS_ERROR;		// Should return quickly.

                    vf->vfs->treeBlockTemp->up = new_root->self;

                    if (PVFS_write_block_tree ( vf->vfs, vf->vfs->treeBlockTemp->self, vf->vfs->treeBlockTemp ) <= 0) return PVFS_ERROR;
                    if (PVFS_write_block_tree ( vf->vfs, new_root->self, new_root ) <= 0) return PVFS_ERROR;

                    // Update the file entry.
                    vf->info.startBlock = new_root->self;
                    if (vf->tableIndex < 0) return PVFS_OK;
                    if (PVFS_read_block_file ( vf->vfs, vf->tableBlock, vf->vfs->fileBlock ) == PVFS_INVALID_LOCATION) return PVFS_ERROR;
                    fileEntry = &vf->vfs->fileBlock->files [ vf->tableIndex ];
                    PVFS_copy_fileEntry ( fileEntry, &(vf->info) );
                    if (PVFS_write_block_file ( vf->vfs, vf->tableBlock, vf->vfs->fileBlock ) <= 0) return PVFS_ERROR;

                    return PVFS_OK;
                }else{
                    return PVFS_ARG_NULL;
                }
            }else{
                return PVFS_ARG_NULL;
            }
            return PVFS_OK;

        }
    }
}

// Adds a mapping to a tree, if neccessary alters the root tree of the file.
std::int32_t PVFS_tree_add ( std::shared_ptr<PvfsFileHandle> &vf, std::shared_ptr<PvfsBlockTree> &tree, std::shared_ptr<PvfsLocationMap> &map )
{
    int64_t					address			=	0;		    // Tree address up from the one passed in.
    PvfsFileEntry*		fileEntry;	                    // Pointer to the file entry.
    std::shared_ptr<PvfsBlockTree>		new_root;		// New root of the tree.

    if ( !vf ) return PVFS_ARG_NULL;
    if ( !tree ) return PVFS_ARG_NULL;
    if ( !map ) return PVFS_ARG_NULL;

    // If there is room in the tree branch just above add to it.
    if ( tree->count < tree->maxMappings )
    {
        tree->mappings [ tree->count ].address		= map->address;
        tree->mappings [ tree->count ].blockLoc	= map->blockLoc;
        tree->count++;
        if (PVFS_write_block_tree ( vf->vfs, tree->self, tree ) <= 0) return PVFS_ERROR;
        return PVFS_OK;
    }
    else // We have to mess with the tree.
    {
        // Allocate a new tree branch.
        // Add the map to that branch.
        // Add that branch to the tree up.
        // If the tree up from the branch is root
        //		create a new root with the old root as the first map.
        //      then add the new branch.
        address = tree->up;

        if (PVFS_allocate_block(vf->vfs) <= 0) return PVFS_ERROR;

        if(!vf->vfs)  return PVFS_ARG_NULL;

        PVFS_cast_block_to_tree ( vf->vfs->block, vf->vfs->treeBlockTemp );

        if(!vf->vfs->treeBlockTemp)  return PVFS_ARG_NULL;

        vf->vfs->treeBlockTemp->up = address;					// Set so the up from this branch is the same as the other.
        if (PVFS_tree_add ( vf, vf->vfs->treeBlockTemp, map ) != PVFS_OK) return PVFS_ERROR;	// Should return quickly from the top part.

        if ( address != PVFS_INVALID_LOCATION )
        {
            map->address	= vf->currentAddress;
            map->blockLoc	= vf->vfs->treeBlockTemp->self;

            if (PVFS_read_block_tree ( vf->vfs, address, vf->tree ) == PVFS_INVALID_LOCATION) return PVFS_ERROR;
            return PVFS_tree_add ( vf, vf->tree, map );
        }
        else	// Create a new root node.
        {
            new_root = create_PVFS_block_tree ( vf->vfs );
            if (PVFS_allocate_block(vf->vfs) <= 0) return PVFS_ERROR;
            PVFS_cast_block_to_tree ( vf->vfs->block, new_root );
            new_root->up = PVFS_INVALID_LOCATION;

            // Old tree has the new root.
            tree->up = new_root->self;
            if (PVFS_write_block_tree ( vf->vfs, tree->self, tree ) <= 0) return PVFS_ERROR;

            // New tree gets the old map.
            map->address	= tree->mappings [ 0 ].address;
            map->blockLoc	= tree->self;
            if (PVFS_tree_add ( vf, new_root, map ) != PVFS_OK) return PVFS_ERROR;		// Should return quickly.

            // Alter the tree passed in to have the new tree as the upper.
            map->address	= vf->currentAddress;
            map->blockLoc	= vf->vfs->treeBlockTemp->self;
            if (PVFS_tree_add ( vf, new_root, map ) != PVFS_OK) return PVFS_ERROR;		// Should return quickly.

            vf->vfs->treeBlockTemp->up = new_root->self;

            if (PVFS_write_block_tree ( vf->vfs, vf->vfs->treeBlockTemp->self, vf->vfs->treeBlockTemp ) <= 0) return PVFS_ERROR;
            if (PVFS_write_block_tree ( vf->vfs, new_root->self, new_root ) <= 0) return PVFS_ERROR;

            // Update the file entry.
            vf->info.startBlock = new_root->self;
                    if (vf->tableIndex < 0) return PVFS_OK;
            if (PVFS_read_block_file ( vf->vfs, vf->tableBlock, vf->vfs->fileBlock ) == PVFS_INVALID_LOCATION) return PVFS_ERROR;
            fileEntry = &vf->vfs->fileBlock->files [ vf->tableIndex ];
            PVFS_copy_fileEntry ( fileEntry, &(vf->info) );
            if (PVFS_write_block_file ( vf->vfs, vf->tableBlock, vf->vfs->fileBlock ) <= 0) return PVFS_ERROR;

            return PVFS_OK;
        }
    }
}

// Utilities for quick file adding and removing for viewing.
std::int32_t PVFS_add ( std::shared_ptr<PvfsFile> &vfs, const char * filename, const char * in_filename )
{
    int					                    fd	=	0;					// File to add.
    std::uint8_t					        buffer [ 1024 ];	        // Buffer to move data around.
    std::shared_ptr<pvfs::PvfsFileHandle>	vf	=	nullptr;			// Internal file pointer.
#ifdef _WIN32
    struct _stati64			                stats;
#else
    struct stat                             stats;
#endif
    std::int64_t					        i	=	0;					// Iterator through everything.
    std::int64_t					        end	=	0;					// When to stop.
    std::int64_t					       rest =	0;					// Rest of the data.

    if ( !vfs ) return PVFS_ARG_NULL;
    if ( filename == nullptr ) return PVFS_ARG_NULL;
    if ( in_filename == nullptr ) return PVFS_ARG_NULL;

#ifdef _WIN32
    fd = _open ( in_filename, _O_BINARY | _O_RDWR, _S_IREAD );
#else
    fd = open(in_filename, O_RDWR, S_IRUSR);
#endif
    if ( fd == PVFS_INVALID_FD ) return PVFS_FILE_NOT_OPENED;
#ifdef _WIN32
    _fstati64 ( fd, &stats );
#else
    fstat( fd, &stats);
#endif

    vf = PVFS_fcreate ( vfs, filename );
    if ( !vf )
    {
#ifdef _WIN32
        _close( fd );
#else
        close( fd );
#endif
        return PVFS_FILE_NOT_OPENED;
    }

    memset ( buffer, 0, 1024 );

    end = stats.st_size;

    for ( i = 0; i < end; i += 1024 )
    {
        if ( i + 1024 < end )
        {
#ifdef _WIN32
            _read ( fd, buffer, 1024 );
#else
             read ( fd, buffer, 1024 );
#endif
            PVFS_write ( vf, buffer, 1024 );
        }
        else
        {
            rest = end - i;
#ifdef _WIN32
            _read (	fd, buffer, (std::uint32_t) rest );
#else
            read (	fd, buffer, (std::uint32_t) rest );
#endif
            PVFS_write ( vf, buffer, (std::int32_t) rest );
        }
    }
#ifdef _WIN32
    _close ( fd );
#else
    close ( fd );
#endif
    PVFS_fclose ( vf );
    return PVFS_OK;
}


namespace {
struct ScopedDescriptor {
    int fd = PVFS_INVALID_FD;
    ~ScopedDescriptor() {
        if (fd != PVFS_INVALID_FD) {
#ifdef _WIN32
            _close(fd);
#else
            close(fd);
#endif
        }
    }
};
}

std::int32_t PVFS_extract(std::shared_ptr<PvfsFile>& vfs, const char* filename, const char* out_filename)
{
    if (!vfs || !filename || !out_filename) return PVFS_ARG_NULL;
    auto file = PVFS_fopen(vfs, filename);
    if (!file || file->info.size < 0) return PVFS_FILE_NOT_OPENED;
    ScopedDescriptor output;
#ifdef _WIN32
    output.fd = _open(out_filename, _O_BINARY | _O_CREAT | _O_TRUNC | _O_WRONLY, _S_IREAD | _S_IWRITE);
#else
    output.fd = open(out_filename, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
#endif
    if (output.fd == PVFS_INVALID_FD) return PVFS_FILE_NOT_OPENED;
    uint8_t buffer[16384];
    int64_t remaining = file->info.size;
    while (remaining > 0) {
        const auto requested = static_cast<uint32_t>(std::min<int64_t>(remaining, sizeof(buffer)));
        const auto count = PVFS_read(file, buffer, requested);
        if (count < 0 || static_cast<uint32_t>(count) != requested) return PVFS_ERROR;
        if (!writeAll(output.fd, buffer, requested)) return PVFS_ERROR;
        remaining -= count;
    }
    return PVFS_OK;
}

// Write a detached replacement first. The live directory entry and its blocks
// remain intact until all replacement data has been flushed successfully.
// The final directory update is NOT power-loss atomic: the PVFS format has no
// metadata journal. On an ambiguous publication failure retain both block sets.
std::int32_t PVFS_overwrite(std::shared_ptr<PvfsFile>& vfs, const char* vfs_filename, const char* disk_filename)
{
    if (!vfs || !vfs_filename || !disk_filename) return PVFS_ARG_NULL;
    if (!*vfs_filename || strlen(vfs_filename) >= PVFS_MAX_FILENAME_LENGTH) return PVFS_ERROR;
    ScopedDescriptor input;
#ifdef _WIN32
    input.fd = _open(disk_filename, _O_BINARY | _O_RDONLY);
    struct _stati64 stats;
    if (input.fd == PVFS_INVALID_FD || _fstati64(input.fd, &stats) != 0) return PVFS_FILE_NOT_OPENED;
#else
    input.fd = open(disk_filename, O_RDONLY);
    struct stat stats;
    if (input.fd == PVFS_INVALID_FD || fstat(input.fd, &stats) != 0) return PVFS_FILE_NOT_OPENED;
#endif
    if (stats.st_size < 0) return PVFS_ERROR;

    // Locate the original entry, or a free directory slot. Use independent
    // buffers because block allocation and tree growth reuse vfs's scratch data.
    auto directory = create_PVFS_block_file(vfs);
    auto selected = create_PVFS_block_file(vfs);
    if (!directory || !selected) return PVFS_ERROR;
    int index = -1;
    bool replacing = false;
    std::set<int64_t> visited;
    for (int64_t address = vfs->tableLoc; address != PVFS_INVALID_LOCATION; address = directory->next) {
        if (!visited.insert(address).second
            || PVFS_read_block_file(vfs, address, directory) == PVFS_INVALID_LOCATION) return PVFS_ERROR;
        for (size_t i = 0; i < directory->files.size(); ++i) {
            const auto& entry = directory->files[i];
            if (strncmp(reinterpret_cast<const char*>(entry.filename), vfs_filename, PVFS_MAX_FILENAME_LENGTH) == 0) {
                *selected = *directory;
                index = static_cast<int>(i);
                replacing = true;
                break;
            }
            if (index < 0 && entry.filename[0] == 0) {
                *selected = *directory;
                index = static_cast<int>(i);
            }
        }
        if (replacing) break;
    }
    if (index < 0) *selected = *directory;
    std::vector<int64_t> oldTrees, oldData;
    if (replacing && PVFS_collect_file_blocks(vfs, selected->files[index].startBlock, oldTrees, oldData) != PVFS_OK)
        return PVFS_ERROR;

    auto staged = create_PVFS_file_handle(vfs);
    if (!staged) return PVFS_ERROR;
    staged->tableIndex = -1; // Detached: flush/tree growth must not publish it.
    const auto treeAddress = PVFS_allocate_block(vfs);
    if (treeAddress <= 0) return PVFS_ERROR;
    staged->tree->self = treeAddress;
    const auto dataAddress = PVFS_allocate_block(vfs);
    if (dataAddress <= 0) return PVFS_ERROR;
    staged->data->self = dataAddress;
    staged->data->tree = treeAddress;
    staged->tree->mappings[0].address = 0;
    staged->tree->mappings[0].blockLoc = dataAddress;
    staged->tree->count = 1;
    staged->info.startBlock = treeAddress;
    staged->info.size = 0;
    strcpy(reinterpret_cast<char*>(staged->info.filename), vfs_filename);
    staged->currentAddress = 0;
    staged->dataAddress = 0;
    staged->dirty = PVFS_DIRTY;

    uint8_t buffer[16384];
    int64_t remaining = stats.st_size;
    while (remaining > 0) {
        const auto requested = static_cast<size_t>(std::min<int64_t>(remaining, sizeof(buffer)));
        const auto count = p_read(input.fd, buffer, requested);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || count > remaining
            || PVFS_write(staged, buffer, static_cast<uint32_t>(count)) != count) return PVFS_ERROR;
        remaining -= count;
    }
    uint8_t extra;
    if (p_read(input.fd, &extra, 1) != 0 || staged->info.size != stats.st_size
        || PVFS_flush(staged, false) != PVFS_OK) return PVFS_ERROR;

    // Only now publish the fully written replacement. Preserve the existing
    // policy: OS write completion here, forced durability at explicit flushes.
    // A failed publication must not recycle either block set.
    const auto before = *selected;
    if (index >= 0) {
        selected->files[index] = staged->info;
    } else if (selected->files.size() < vfs->fileMaxCount) {
        selected->files.push_back(staged->info);
    } else {
        auto next = create_PVFS_block_file(vfs);
        if (!next) return PVFS_ERROR;
        next->self = PVFS_allocate_block(vfs);
        if (next->self <= 0) return PVFS_ERROR;
        next->prev = selected->self;
        next->files.push_back(staged->info);
        if (PVFS_write_block_file(vfs, next->self, next) <= 0) return PVFS_ERROR;
        selected->next = next->self;
    }
    if (PVFS_write_block_file(vfs, selected->self, selected) <= 0) {
        // Best effort rollback for recoverable I/O errors; never claim success.
        *selected = before;
        PVFS_write_block_file(vfs, selected->self, selected);
        return PVFS_ERROR;
    }

    // Reclaim only after publication. Failed pre-publication writes can leak
    // unreachable blocks, deliberately preferring space loss to data loss.
    for (const auto address : oldData)
        if (PVFS_add_free_block(vfs, address, vfs->blockSize) != PVFS_OK) return PVFS_ERROR;
    for (const auto address : oldTrees)
        if (PVFS_add_free_block(vfs, address, vfs->blockSize) != PVFS_OK) return PVFS_ERROR;
    return PVFS_OK;
}


std::int64_t p_write(int fd, void *buf, size_t count)
{
#ifdef _WIN32
    return _write ( fd, buf, count );
#else
    return write ( fd, buf, count );
#endif
}

std::int64_t p_read(int fd, void *buf, size_t count)
{
#ifdef _WIN32
    return _read ( fd, buf, count );
#else
    return read ( fd, buf, count );
#endif
}

std::int32_t PVFS_read_index_file_header ( std::shared_ptr<pvfs::PvfsFileHandle> &file_handle, pvfs::PvfsIndexHeader &header )
{
    if ( !file_handle )return pvfs::PVFS_ERROR;

    PVFS_seek ( file_handle, 0 );
    PVFS_fread_uint32 ( file_handle, &header.magicNumber );
    PVFS_fread_uint32 ( file_handle, &header.version );
    PVFS_fread_uint32 ( file_handle, &header.dataType );
    PVFS_fread_float  ( file_handle, &header.datarate );
    PVFS_fread_sint64 ( file_handle, &header.startTime.seconds );
    PVFS_fread_double ( file_handle, &header.startTime.subSeconds );
    PVFS_fread_sint64 ( file_handle, &header.endTime.seconds );
    PVFS_fread_double ( file_handle, &header.endTime.subSeconds );
    PVFS_fread_uint32 ( file_handle, &header.timeStampIntervalSeconds );

    return pvfs::PVFS_OK;
}

std::int32_t PVFS_write_index_file_header ( std::shared_ptr<pvfs::PvfsFileHandle> &file_handle, pvfs::PvfsIndexHeader &header )
{
    if ( !file_handle )return pvfs::PVFS_ERROR;
    PVFS_seek ( file_handle, 0 );
    PVFS_fwrite_uint32 ( file_handle, header.magicNumber );
    PVFS_fwrite_uint32 ( file_handle, header.version );
    PVFS_fwrite_uint32 ( file_handle, header.dataType );
    PVFS_fwrite_float  ( file_handle, header.datarate );
    PVFS_fwrite_sint64 ( file_handle, header.startTime.seconds );
    PVFS_fwrite_double ( file_handle, header.startTime.subSeconds );
    PVFS_fwrite_sint64 ( file_handle, header.endTime.seconds );
    PVFS_fwrite_double ( file_handle, header.endTime.subSeconds );
    PVFS_fwrite_uint32 ( file_handle, header.timeStampIntervalSeconds );

    return pvfs::PVFS_OK;
}

std::int64_t PVFS_write_uint8 ( int fd, std::uint8_t value ) { return p_write ( fd, &value, sizeof ( std::uint8_t )); }
std::int64_t PVFS_read_uint8 ( int fd, std::uint8_t & value ) { return p_read ( fd, &value, sizeof ( std::uint8_t )); }
std::int64_t PVFS_write_sint8 ( int fd, std::int8_t value ) { return p_write ( fd, &value, sizeof ( std::int8_t )); }
std::int64_t PVFS_read_sint8 ( int fd, std::int8_t & value ) { return p_read ( fd, &value, sizeof ( std::int8_t )); }
std::int64_t PVFS_write_uint16 ( int fd, std::uint16_t value ) { return p_write ( fd, &value, sizeof ( std::uint16_t )); }
std::int64_t PVFS_read_uint16 ( int fd, std::uint16_t & value ) { return p_read ( fd, &value, sizeof ( std::uint16_t )); }
std::int64_t PVFS_write_sint16 ( int fd, std::int16_t value ) { return p_write ( fd, &value, sizeof (std::int16_t )); }
std::int64_t PVFS_read_sint16 ( int fd, std::int16_t & value ) { return p_read ( fd, &value, sizeof ( std::int16_t )); }
std::int64_t PVFS_write_uint32 ( int fd, std::uint32_t value ) { return p_write ( fd, &value, sizeof ( std::uint32_t )); }
std::int64_t PVFS_read_uint32 ( int fd, std::uint32_t & value ) { return p_read ( fd, &value, sizeof ( std::uint32_t )); }
std::int64_t PVFS_write_sint32 ( int fd, std::int32_t value ) { return p_write ( fd, &value, sizeof ( std::int32_t )); }
std::int64_t PVFS_read_sint32 ( int fd, std::int32_t & value ) { return p_read ( fd, &value, sizeof ( std::int32_t )); }
std::int64_t PVFS_write_sint64 ( int fd, std::int64_t value ) { return p_write ( fd, &value, sizeof ( std::int64_t )); }
std::int64_t PVFS_read_sint64 ( int fd, std::int64_t & value ) { return p_read ( fd, &value, sizeof ( std::int64_t )); }


std::int64_t PVFS_fwrite_uint8 ( std::shared_ptr<pvfs::PvfsFileHandle> &file, std::uint8_t value ) { return PVFS_write ( file, reinterpret_cast<uint8_t *>(&value), sizeof ( std::uint8_t )); }
std::int64_t PVFS_fread_uint8 ( std::shared_ptr<pvfs::PvfsFileHandle> &file, std::uint8_t * value ) { return PVFS_read ( file, reinterpret_cast<uint8_t *>(value), sizeof ( std::uint8_t )); }
std::int64_t PVFS_fwrite_sint8 ( std::shared_ptr<pvfs::PvfsFileHandle> &file, std::int8_t value ) { return PVFS_write ( file, reinterpret_cast<uint8_t *>(&value), sizeof ( std::int8_t )); }
std::int64_t PVFS_fread_sint8 ( std::shared_ptr<pvfs::PvfsFileHandle> &file, std::int8_t * value ) { return PVFS_read ( file, reinterpret_cast<uint8_t *>(value), sizeof ( std::int8_t )); }
std::int64_t PVFS_fwrite_uint16 ( std::shared_ptr<pvfs::PvfsFileHandle> &file, std::uint16_t value ) { return PVFS_write ( file, reinterpret_cast<uint8_t *>(&value), sizeof ( std::uint16_t )); }
std::int64_t PVFS_fread_uint16 ( std::shared_ptr<pvfs::PvfsFileHandle> &file, std::uint16_t * value ) { return PVFS_read ( file, reinterpret_cast<uint8_t *>(value), sizeof ( std::uint16_t )); }
std::int64_t PVFS_fwrite_sint16 ( std::shared_ptr<pvfs::PvfsFileHandle> &file, std::int16_t value ) { return PVFS_write ( file, reinterpret_cast<uint8_t *>(&value), sizeof ( std::int16_t )); }
std::int64_t PVFS_fread_sint16 ( std::shared_ptr<pvfs::PvfsFileHandle> &file, std::int16_t * value ) { return PVFS_read ( file, reinterpret_cast<uint8_t *>(value), sizeof ( std::int16_t )); }
std::int64_t PVFS_fwrite_uint32 ( std::shared_ptr<pvfs::PvfsFileHandle> &file, std::uint32_t value ) { return PVFS_write ( file, reinterpret_cast<uint8_t *>(&value), sizeof ( std::uint32_t )); }
std::int64_t PVFS_fread_uint32 ( std::shared_ptr<pvfs::PvfsFileHandle> &file, std::uint32_t * value ) { return PVFS_read ( file, reinterpret_cast<uint8_t *>(value), sizeof ( std::uint32_t )); }
std::int64_t PVFS_fwrite_sint32 ( std::shared_ptr<pvfs::PvfsFileHandle> &file, std::int32_t value ) { return PVFS_write ( file, reinterpret_cast<uint8_t *>(&value), sizeof ( std::int32_t )); }
std::int64_t PVFS_fread_sint32 ( std::shared_ptr<pvfs::PvfsFileHandle> &file, std::int32_t * value ) { return PVFS_read ( file, reinterpret_cast<uint8_t *>(value), sizeof ( std::int32_t )); }
std::int64_t PVFS_fwrite_sint64 ( std::shared_ptr<pvfs::PvfsFileHandle> &file, std::int64_t value ) { return PVFS_write ( file, reinterpret_cast<uint8_t *>(&value), sizeof ( std::int64_t )); }
std::int64_t PVFS_fread_sint64 ( std::shared_ptr<pvfs::PvfsFileHandle> &file, std::int64_t * value ) { return PVFS_read ( file, reinterpret_cast<uint8_t *>(value), sizeof ( std::int64_t )); }
std::int64_t PVFS_fwrite_float ( std::shared_ptr<pvfs::PvfsFileHandle> &file, float value ) { return PVFS_write ( file, reinterpret_cast<uint8_t *>(&value), sizeof ( float )); }
std::int64_t PVFS_fread_float ( std::shared_ptr<pvfs::PvfsFileHandle> &file, float * value ) { return PVFS_read ( file, reinterpret_cast<uint8_t *>(value), sizeof ( float )); }
std::int64_t PVFS_fwrite_double ( std::shared_ptr<pvfs::PvfsFileHandle> &file, double value ) { return PVFS_write ( file, reinterpret_cast<uint8_t *>(&value), sizeof ( double )); }
std::int64_t PVFS_fread_double ( std::shared_ptr<pvfs::PvfsFileHandle> &file, double * value ) { return PVFS_read ( file, reinterpret_cast<uint8_t *>(value), sizeof ( double )); }

void PVFS_lock(std::shared_ptr<pvfs::PvfsFile> &vfs)
{
    if(!vfs)return;
    vfs->lock.lock();
}

void PVFS_unlock(std::shared_ptr<pvfs::PvfsFile> &vfs)
{
    if(!vfs)return;
    vfs->lock.unlock();
}

void PVFS_lock_file(std::shared_ptr<pvfs::PvfsFileHandle> &vf)
{
    if(!vf)return;
    vf->vfs->lock.lock();
}

void PVFS_unlock_file(std::shared_ptr<pvfs::PvfsFileHandle> &vf)
{
    if(!vf)return;
    vf->vfs->lock.unlock();
}

}
