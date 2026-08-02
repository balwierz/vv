class Vv < Formula
  desc "Universal genomic file viewer (Parquet, BAM, VCF, BED, GFF, FASTA, FASTQ, ...)"
  homepage "https://github.com/balwierz/vv"
  url "https://github.com/balwierz/vv/archive/refs/tags/v1.4.0.tar.gz"
  # Replace with the sha256 of the release tarball:
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  license "MIT"
  head "https://github.com/balwierz/vv.git", branch: "main"

  depends_on "cmake"        => :build
  depends_on "pkg-config"   => :build
  depends_on "apache-arrow"
  depends_on "htslib"
  depends_on "ncurses"

  def install
    system "cmake", "-S", ".", "-B", "build",
                    "-DCMAKE_BUILD_TYPE=Release",
                    *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  test do
    assert_match "vv ", shell_output("#{bin}/vv --version")
    assert_match "universal genomic file viewer", shell_output("#{bin}/vv --help 2>&1")
  end
end
