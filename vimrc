" All system-wide defaults are set in $VIMRUNTIME/debian.vim and sourced by
" the call to :runtime you can find below.  If you wish to change any of those
" settings, you should do it in this file (/etc/vim/vimrc), since debian.vim
" will be overwritten everytime an upgrade of the vim packages is performed.
" It is recommended to make changes after sourcing debian.vim since it alters
" the value of the 'compatible' option.

" This line should not be removed as it ensures that various options are
" properly set to work with the Vim-related packages available in Debian.
runtime! debian.vim

" Uncomment the next line to make Vim more Vi-compatible
" NOTE: debian.vim sets 'nocompatible'.  Setting 'compatible' changes numerous
" options, so any other options should be set AFTER setting 'compatible'.
"set compatible

" Vim5 and later versions support syntax highlighting. Uncommenting the next
" line enables syntax highlighting by default.
if has("syntax")
  syntax on
endif

" If using a dark background within the editing area and syntax highlighting
" turn on this option as well
"set background=dark

" Uncomment the following to have Vim jump to the last position when
" reopening a file
"if has("autocmd")
"  au BufReadPost * if line("'\"") > 1 && line("'\"") <= line("$") | exe "normal! g'\"" | endif
"endif

" Uncomment the following to have Vim load indentation rules and plugins
" according to the detected filetype.
"if has("autocmd")
"  filetype plugin indent on
"endif

" The following are commented out as they cause vim to behave a lot
" differently from regular Vi. They are highly recommended though.
"set showcmd		" Show (partial) command in status line.
"set showmatch		" Show matching brackets.
"set ignorecase		" Do case insensitive matching
"set smartcase		" Do smart case matching
"set incsearch		" Incremental search
"set autowrite		" Automatically save before commands like :next and :make
"set hidden		" Hide buffers when they are abandoned
set mouse=a		" Enable mouse usage (all modes)
set number
set relativenumber
set ts=8
set nocscopeverbose
set hlsearch        " highlight the search patten
set autoindent
set incsearch
"set cursorline          " highlight line
" auto bracket completion
:inoremap ( ()<ESC>i
:inoremap ) <c-r>=ClosePair(')')<CR>
:inoremap { {<CR>}<ESC>O
:inoremap } <c-r>=ClosePair('}')<CR>
:inoremap [ []<ESC>i
:inoremap ] <c-r>=ClosePair(']')<CR>
:inoremap " ""<ESC>i
:inoremap ' ''<ESC>i
function! ClosePair(char)
    if getline('.')[col('.') - 1] == a:char
        return "\<Right>"
    else
        return a:char
    endif
endfunction
nmap <F2> :q!<CR>
nmap <C-p> :!ctags -R --c++-kinds=+p --fields=+iaS --extra=+q .
nmap <C-o> :!cscope -Rbq
nmap w= :resize +3<CR>
nmap w- :resize -3<CR>
nmap w, :vertical resize -3<CR>
nmap w. :vertical resize +3<CR>
set hlsearch        " highlight the search patten
nmap <S-p> :nohlsearch<CR>

" colorscheme
" set background=dark
set t_Co=256
" colorscheme solarized
" colorscheme desert
colorscheme darkblue
" colorscheme murphy

" Source a global configuration file if available
if filereadable("/etc/vim/vimrc.local")
  source /etc/vim/vimrc.local
endif

" Taglist
nmap <F7> :TlistToggle<CR>
let Tlist_Show_One_File=1
let Tlist_Exit_OnlyWindow=1

" Quickfix
set cscopequickfix=s-,c-,d-,i-,t-,e-
nmap <F4> :cp<CR>
nmap <F5> :cn<CR>
" nmap <C-t> :colder<CR>:cc<CR>

" OmniCppComplete
set nocp
filetype plugin on
set completeopt=menu " close preview window

" superTab
let g:SuperTabDefaultCompletionType="context"

" NERDTree
let NERDTreeWinPos = "right"
nmap <F9> :NERDTreeToggle<CR>


" mini buffer related
let g:miniBufExplMapWindowNavVim = 1   
let g:miniBufExplMapWindowNavArrows = 1   
let g:miniBufExplMapCTabSwitchBufs = 1   " open the selected file
let g:miniBufExplModSelTarget = 1
let g:miniBufExplMoreThanOne = 0

" grep
nnoremap <silent> <F3> :Grep<CR>

" switch file between c/h
nmap <S-A> :A<CR>

" Source Explorer
nmap <F12> :SrcExplToggle<CR>
let g:SrcExpl_winHeight = 12
let g:SrcExpl_refreshTime = 100
let g:SrcExpl_jumpKey = "<ENTER>"
let g:SrcExpl_gobackKey = "<SPACE>" 
let g:SrcExpl_isUpdateTags = 0 

" Vundle manage
set nocompatible              " be iMproved, required
filetype off                  " required

" set the runtime path to include Vundle and initialize
set rtp+=~/.vim/bundle/Vundle.vim
call vundle#begin()

" let Vundle manage Vundle, required
Plugin 'VundleVim/Vundle.vim'
"Plugin 'Valloric/YouCompleteMe'
"Plugin 'scrooloose/nerdtree'
"Plugin 'majutsushi/tagbar' " Tag bar"
"Plugin 'Xuyuanp/nerdtree-git-plugin'
"Plugin 'jistr/vim-nerdtree-tabs'
"Plugin 'vim-airline/vim-airline' | Plugin 'vim-airline/vim-airline-themes' " Status line"
"Plugin 'jiangmiao/auto-pairs'
"Plugin 'mbbill/undotree'
"Plugin 'gdbmgr'
"Plugin 'scrooloose/nerdcommenter'
"Plugin 'Yggdroot/indentLine' " Indentation level"
"Plugin 'bling/vim-bufferline' " Buffer line"
"Plugin 'kepbod/quick-scope' " Quick scope
"Plugin 'yianwillis/vimcdoc'
"Plugin 'nelstrom/vim-visual-star-search'
"Plugin 'ludovicchabant/vim-gutentags'
"Plugin 'w0rp/ale'
"Plugin 'mbbill/echofunc'
"Plugin 'Yggdroot/LeaderF', { 'do': './install.sh' }

" All of your Plugins must be added before the following line
call vundle#end()            " required
filetype plugin indent on    " required
