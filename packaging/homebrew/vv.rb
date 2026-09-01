class Vv < Formula
  desc "Universal genomic file viewer (Parquet, BAM, VCF, BED, GFF, FASTA, FASTQ, ...)"
  homepage "https://github.com/balwierz/vv"
  url "https://github.com/balwierz/vv/archive/refs/tags/v1.20.0.tar.gz"
  sha256 "5247bab43a80c7ffadad908f9dae031ba9af2c1776dc99f8c99b4b657fc254f5"
  license "MIT"
  head "https://github.com/balwierz/vv.git", branch: "main"

  depends_on "cmake"      => :build
  depends_on "pkgconf"    => :build
  # Every one of these is a hard dependency: CMake aborts at configure time if
  # any is missing (find_package(... REQUIRED) or an explicit FATAL_ERROR).
  # The formula used to declare only arrow/htslib/ncurses and could not get
  # past configure.
  depends_on "apache-arrow"
  depends_on "expat"
  depends_on "hdf5"
  depends_on "htslib"
  depends_on "minizip"
  depends_on "ncurses"
  depends_on "xlsxio"

  def install
    # Every dependency lives under its own prefix, and ncurses is keg-only —
    # so it is NOT on the default search path and find_package(Curses) would
    # silently resolve to Apple's ncurses 5.7, which lacks set_escdelay() and
    # BUTTON5_PRESSED. CMakeLists.txt now fails loudly on that, but naming the
    # prefixes is what actually makes the build work.
    prefixes = %w[apache-arrow htslib ncurses xlsxio expat minizip hdf5]
                 .map { |f| Formula[f].opt_prefix.to_s }

    system "cmake", "-S", ".", "-B", "build",
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DCMAKE_PREFIX_PATH=#{prefixes.join(";")}",
                    *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"

    # No *_completion.install here on purpose: CMakeLists.txt already installs
    # to share/bash-completion/completions, share/fish/vendor_completions.d and
    # share/zsh/site-functions, all three of which Homebrew links. Adding them
    # again would install two copies of each.
  end

  test do
    assert_match "vv ", shell_output("#{bin}/vv --version")
    assert_match "universal genomic file viewer", shell_output("#{bin}/vv --help 2>&1")

    # Actually read a file, not just print help: write a TSV and check vv
    # reports the right shape and both column names.
    (testpath/"t.tsv").write("chrom\tstart\tend\nchr1\t100\t200\nchr1\t300\t400\n")
    assert_equal "2", shell_output("#{bin}/vv --count #{testpath}/t.tsv").strip
    assert_match "chrom", shell_output("#{bin}/vv --list-columns #{testpath}/t.tsv")

    # And that the vh symlink was installed and implies --vertical.
    assert_match "vv ", shell_output("#{bin}/vh --version")
  end
end
