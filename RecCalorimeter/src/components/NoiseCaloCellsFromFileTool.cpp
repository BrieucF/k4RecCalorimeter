#include "NoiseCaloCellsFromFileTool.h"

// FCCSW
#include "DetCommon/DetUtils.h"
#include "k4Interface/IGeoSvc.h"

// DD4hep
#include "DD4hep/Detector.h"

// Root
#include "TFile.h"
#include "TH1F.h"
#include "TMath.h"

DECLARE_COMPONENT(NoiseCaloCellsFromFileTool)

NoiseCaloCellsFromFileTool::NoiseCaloCellsFromFileTool(const std::string& type, const std::string& name,
                                                       const IInterface* parent)
    : GaudiTool(type, name, parent), m_geoSvc("GeoSvc", name) {
  declareInterface<INoiseCaloCellsTool>(this);
  declareProperty("cellPositionsTool", m_cellPositionsTool, "Handle for tool to retrieve cell positions");
}

StatusCode NoiseCaloCellsFromFileTool::initialize() {
  
  if (!m_geoSvc) {
    error() << "Unable to locate Geometry Service. "
            << "Make sure you have GeoSvc and SimSvc in the right order in the configuration." << endmsg;
    return StatusCode::FAILURE;
  }

  // Initialize random service
  if (service("RndmGenSvc", m_randSvc).isFailure()) {
    error() << "Couldn't get RndmGenSvc!!!!" << endmsg;
    return StatusCode::FAILURE;
  }

  if (m_gauss.initialize(m_randSvc, Rndm::Gauss(0., 1.)).isFailure()) {
    error() << "Couldn't initialize RndmGenSvc!!!!" << endmsg;
    return StatusCode::FAILURE;
  }


  // open and check file, read the histograms with noise constants
  if (initNoiseFromFile().isFailure()) {
    error() << "Couldn't open file with noise constants!!!" << endmsg;
    return StatusCode::FAILURE;
  }
  // Check if cell position tool available
  if (!m_cellPositionsTool.retrieve() and !m_useSeg) {
    info() << "Unable to retrieve cell positions tool, try eta-phi segmentation." << endmsg;
    // Get PhiEta segmentation
    m_segmentationPhiEta = dynamic_cast<dd4hep::DDSegmentation::FCCSWGridPhiEta*>(
									    m_geoSvc->lcdd()->readout(m_readoutName).segmentation().segmentation());
    if (m_segmentationPhiEta == nullptr) {
      error() << "There is no phi-eta segmentation." << endmsg;
      return StatusCode::FAILURE;
    }
    else
      info() << "Found phi-eta segmentation." << endmsg;
  }    
  // Get PhiEta segmentation
  m_segmentationPhiEta = dynamic_cast<dd4hep::DDSegmentation::FCCSWGridPhiEta*>(
    m_geoSvc->lcdd()->readout(m_readoutName).segmentation().segmentation());
  if (m_segmentationPhiEta == nullptr) {
    m_segmentationMulti = dynamic_cast<dd4hep::DDSegmentation::MultiSegmentation*>(
      m_geoSvc->lcdd()->readout(m_readoutName).segmentation().segmentation());
    if (m_segmentationMulti == nullptr) {
      error() << "There is no phi-eta or multi- segmentation for the readout " << m_readoutName << " defined." << endmsg;
      return StatusCode::FAILURE;
    } else {
      // check if multisegmentation contains only phi-eta sub-segmentations
      const dd4hep::DDSegmentation::FCCSWGridPhiEta* subsegmentation = nullptr;
      for (const auto& subSegm: m_segmentationMulti->subSegmentations()) {
        subsegmentation = dynamic_cast<dd4hep::DDSegmentation::FCCSWGridPhiEta*>(subSegm.segmentation);
        if (subsegmentation == nullptr) {
          error() << "At least one of the sub-segmentations in MultiSegmentation named " << m_readoutName << " is not a phi-eta grid." << endmsg;
          return StatusCode::FAILURE;
        }
      }
    }
  }

  debug() << "Filter noise threshold: " << m_filterThreshold << "*sigma" << endmsg;

  StatusCode sc = GaudiTool::initialize();
  if (sc.isFailure()) return sc;

  return sc;
}

void NoiseCaloCellsFromFileTool::addRandomCellNoise(std::unordered_map<uint64_t, double>& aCells) {
  std::for_each(aCells.begin(), aCells.end(), [this](std::pair<const uint64_t, double>& p) {
    p.second += (getNoiseConstantPerCell(p.first) * m_gauss.shoot());
  });
}

void NoiseCaloCellsFromFileTool::filterCellNoise(std::unordered_map<uint64_t, double>& aCells) {
  // Erase a cell if it has energy bellow a threshold from the vector
  auto it = aCells.begin();
  while ((it = std::find_if(it, aCells.end(), [this](std::pair<const uint64_t, double>& p) {
            return bool(p.second < m_filterThreshold * getNoiseConstantPerCell(p.first));
          })) != aCells.end()) {
    aCells.erase(it++);
  }
}

StatusCode NoiseCaloCellsFromFileTool::finalize() {
  StatusCode sc = GaudiTool::finalize();
  return sc;
}

StatusCode NoiseCaloCellsFromFileTool::initNoiseFromFile() {
  // check if file exists
  if (m_noiseFileName.empty()) {
    error() << "Name of the file with noise values not set" << endmsg;
    return StatusCode::FAILURE;
  }
  std::unique_ptr<TFile> noiseFile(TFile::Open(m_noiseFileName.value().c_str(), "READ"));
  if (noiseFile->IsZombie()) {
    error() << "Couldn't open the file with noise constants" << endmsg;
    return StatusCode::FAILURE;
  } else {
    info() << "Opening the file with noise constants: " << m_noiseFileName << endmsg;
  }

  std::string elecNoiseLayerHistoName, pileupLayerHistoName;
  // Read the histograms with electronics noise and pileup from the file
  for (unsigned i = 0; i < m_numRadialLayers; i++) {
    elecNoiseLayerHistoName = m_elecNoiseHistoName + std::to_string(i + 1);
    debug() << "Getting histogram with a name " << elecNoiseLayerHistoName << endmsg;
    m_histoElecNoiseConst.push_back(*dynamic_cast<TH1F*>(noiseFile->Get(elecNoiseLayerHistoName.c_str())));
    if (m_histoElecNoiseConst.at(i).GetNbinsX() < 1) {
      error() << "Histogram  " << elecNoiseLayerHistoName
              << " has 0 bins! check the file with noise and the name of the histogram!" << endmsg;
      return StatusCode::FAILURE;
    }
    if (m_addPileup) {
      pileupLayerHistoName = m_pileupHistoName + std::to_string(i + 1);
      debug() << "Getting histogram with a name " << pileupLayerHistoName << endmsg;
      m_histoPileupConst.push_back(*dynamic_cast<TH1F*>(noiseFile->Get(pileupLayerHistoName.c_str())));
      if (m_histoPileupConst.at(i).GetNbinsX() < 1) {
        error() << "Histogram  " << pileupLayerHistoName
                << " has 0 bins! check the file with noise and the name of the histogram!" << endmsg;
        return StatusCode::FAILURE;
      }
    }
  }

  noiseFile->Close();

  // Check if we have same number of histograms (all layers) for pileup and electronics noise
  if (m_histoElecNoiseConst.size() == 0) {
    error() << "No histograms with noise found!!!!" << endmsg;
    return StatusCode::FAILURE;
  }
  if (m_addPileup) {
    if (m_histoElecNoiseConst.size() != m_histoPileupConst.size()) {
      error() << "Missing histograms! Different number of histograms for electronics noise and pileup!!!!" << endmsg;
      return StatusCode::FAILURE;
    }
  }

  return StatusCode::SUCCESS;
}

double NoiseCaloCellsFromFileTool::getNoiseConstantPerCell(int64_t aCellId) {
  const dd4hep::DDSegmentation::FCCSWGridPhiEta* segmentation = m_segmentationPhiEta;
  if (segmentation == nullptr) {
    segmentation = dynamic_cast<const dd4hep::DDSegmentation::FCCSWGridPhiEta*>(&m_segmentationMulti->subsegmentation(aCellId));
  }

  double elecNoise = 0.;
  double pileupNoise = 0.;

  // Take readout, bitfield from GeoSvc
  auto decoder = m_geoSvc->lcdd()->readout(m_readoutName).idSpec().decoder();
  dd4hep::DDSegmentation::CellID cID = aCellId;
 
  double cellEta;
  if (m_useSeg)
    cellEta = m_segmentationPhiEta->eta(aCellId);
  else
    cellEta = m_cellPositionsTool->xyzPosition(cID).Eta();
  unsigned cellLayer = decoder->get(cID, m_activeFieldName);

  // All histograms have same binning, all bins with same size
  // Using the histogram in the first layer to get the bin size
  unsigned index = 0;
  if (m_histoElecNoiseConst.size() != 0) {
    int Nbins = m_histoElecNoiseConst.at(index).GetNbinsX();
    double deltaEtaBin =
        (m_histoElecNoiseConst.at(index).GetBinLowEdge(Nbins) + m_histoElecNoiseConst.at(index).GetBinWidth(Nbins) -
         m_histoElecNoiseConst.at(index).GetBinLowEdge(1)) /
        Nbins;
    double etaFirtsBin = m_histoElecNoiseConst.at(index).GetBinLowEdge(1);
    // find the eta bin for the cell
    int ibin = floor((fabs(cellEta) - etaFirtsBin) / deltaEtaBin) + 1;
    if (ibin > Nbins) {
      debug() << "eta outside range of the histograms! Cell eta: " << cellEta << " Nbins in histogram: " << Nbins
              << endmsg;
      ibin = Nbins;
    }
    // Check that there are not more layers than the constants are provided for
    if (cellLayer < m_histoElecNoiseConst.size()) {
      elecNoise = m_histoElecNoiseConst.at(cellLayer).GetBinContent(ibin);
      if (m_addPileup) {
        pileupNoise = m_histoPileupConst.at(cellLayer).GetBinContent(ibin);
      }
    } else {
      debug()
          << "More radial layers than we have noise for!!!! Using the last layer for all histograms outside the range."
          << endmsg;
    }
  } else {
    debug() << "No histograms with noise constants!!!!! " << endmsg;
  }

  // Total noise: electronics noise + pileup
  double totalNoise = sqrt(pow(elecNoise, 2) + pow(pileupNoise, 2));

  if (totalNoise < 1e-3) {
    debug() << "Zero noise: cell eta " << cellEta << " layer " << cellLayer << " noise " << totalNoise << endmsg;
  }

  return totalNoise;
}
