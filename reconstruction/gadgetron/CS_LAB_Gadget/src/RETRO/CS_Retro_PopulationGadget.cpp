#include "CS_Retro_PopulationGadget.h"

using namespace Gadgetron;

// class constructor
CS_Retro_PopulationGadget::CS_Retro_PopulationGadget()
{
}

// class destructor - delete temporal buffer/memory
CS_Retro_PopulationGadget::~CS_Retro_PopulationGadget()
{
}

// read flexible data header
int CS_Retro_PopulationGadget::process_config(ACE_Message_Block *mb)
{
	// set properties
#ifdef __GADGETRON_VERSION_HIGHER_3_6__
	GlobalVar::instance()->iPopulationMode_		= PopulationMode.value();
	GlobalVar::instance()->iGatingMode_			= GatingMode.value();
	fTolerance_ = Tolerance.value();
#else
	GlobalVar::instance()->iPopulationMode_		= *(get_int_value("PopulationMode").get());
	GlobalVar::instance()->iGatingMode_			= *(get_int_value("GatingMode").get());
	fTolerance_ = *(get_int_value("Tolerance").get());
#endif

	return GADGET_OK;
}

int CS_Retro_PopulationGadget::process(GadgetContainerMessage<ISMRMRD::ImageHeader> *m1,GadgetContainerMessage<hoNDArray<float> > *m2, GadgetContainerMessage<hoNDArray<std::complex<float> > > *m3)
{
	iNoChannels_ = m3->getObjectPtr()->get_size(2);

	// get number of phases/gates
	unsigned int number_of_phases = m1->getObjectPtr()->user_int[0];

	// get navigator and convert to std::vector
	//hafNav_ = *m2->getObjectPtr();
	for (unsigned int iI = 0; iI < m2->getObjectPtr()->get_size(0); iI++) {
		vNavInt_.push_back(m2->getObjectPtr()->at(iI));//hafNav_(iI));
	}

	// get unordered kspace data
	hacfKSpace_unordered_.create(m3->getObjectPtr()->get_dimensions());
	memcpy(hacfKSpace_unordered_.get_data_ptr(), m3->getObjectPtr()->get_data_ptr(), sizeof(std::complex<float>)*m3->getObjectPtr()->get_number_of_elements());

	// free memory (also frees m3)
	m2->release();

	// initialize output k-space array (ReadOut x PhaseEncoding x PArtitionencoding x Gates x Channels)
	hacfKSpace_reordered_.create(hacfKSpace_unordered_.get_size(0), m1->getObjectPtr()->matrix_size[1], m1->getObjectPtr()->matrix_size[2], number_of_phases, iNoChannels_);

	//-------------------------------------------------------------------------
	// discard first seconds of the acquisitions and wait for steady-state
	//-------------------------------------------------------------------------
	if (!fDiscard()) {
		GERROR("process aborted\n");
		return GADGET_FAIL;
	}

	//-------------------------------------------------------------------------
	// get centroids
	//-------------------------------------------------------------------------
	if (!fCalcCentroids(number_of_phases)) {
		GERROR("process aborted\n");
		return GADGET_FAIL;
	} else {
		for (size_t i = 0; i < vfCentroids_.size(); i++) {
			GDEBUG("Centroid %i: %f\n", i, vfCentroids_.at(i));
		}
	}

	//-------------------------------------------------------------------------
	// populate k-space: mode: closest, gates: 4
	//-------------------------------------------------------------------------
	if (!fPopulatekSpace(number_of_phases)) {
		GERROR("process aborted\n");
	}

	GINFO("kSpace populated and ready to stream..\n");
	hacfKSpace_reordered_.print(std::cout);

	//-------------------------------------------------------------------------
	// make new GadgetContainer
	//-------------------------------------------------------------------------
	// create container
	GadgetContainerMessage<hoNDArray<std::complex<float> > > *tmp_m2 = new GadgetContainerMessage<hoNDArray<std::complex<float> > >();

	// concat
	m1->cont(tmp_m2);

	// create
	try{
		tmp_m2->getObjectPtr()->create(hacfKSpace_reordered_.get_dimensions());
		memcpy(tmp_m2->getObjectPtr()->get_data_ptr(), hacfKSpace_reordered_.get_data_ptr(), sizeof(std::complex<float>)*hacfKSpace_reordered_.get_number_of_elements());
	} catch (std::runtime_error &err) {
		GEXCEPTION(err, "Unable to allocate new image array\n");
		m1->release();
		return GADGET_FAIL;
	}

	// put on q
	if (this->next()->putq(m1) < 0) {
		return GADGET_FAIL;
	}

	return GADGET_OK;
}

bool CS_Retro_PopulationGadget::fDiscard()
{
	float fPreCrop = 5; // [s]
	int iStartIndex = std::floor(fPreCrop/(GlobalVar::instance()->fTR_/1000));

	// Set iStartIndex to 0 when it is negative
	if (iStartIndex < 0) {
		GWARN("iStartIndex=%d < 0. It is set to 0, maybe you want to check your code?\n", iStartIndex);
		iStartIndex = 0;
	}

	// only erase data when there is enough
	if (vNavInt_.size() >= static_cast<size_t>(iStartIndex)) {
		vNavInt_.erase(vNavInt_.begin(), vNavInt_.begin()+iStartIndex);
	} else {
		GWARN("more elements should be delete than there were actually in vNavInt_. Nothing is done!\n");
	}

	if (GlobalVar::instance()->vPA_.size() >= static_cast<size_t>(iStartIndex)) {
		GlobalVar::instance()->vPA_.erase(GlobalVar::instance()->vPA_.begin(), GlobalVar::instance()->vPA_.begin() + iStartIndex);
	} else {
		GWARN("more elements should be delete than there were actually in vPA_. Nothing is done!\n");
	}

	if (GlobalVar::instance()->vPE_.size() >= static_cast<size_t>(iStartIndex)) {
		GlobalVar::instance()->vPE_.erase(GlobalVar::instance()->vPE_.begin(), GlobalVar::instance()->vPE_.begin() + iStartIndex);
	} else {
		GWARN("more elements should be delete than there were actually in vPE_. Nothing is done!\n");
	}

	GINFO("first seconds discarded - %i samples erased - TR: %f..\n", iStartIndex, GlobalVar::instance()->fTR_);

	// new array size
	std::vector<size_t> vtDims_new = *hacfKSpace_unordered_.get_dimensions();
	vtDims_new.at(1) = vtDims_new.at(1) - iStartIndex;

	GINFO("kSpace before deletion\n");
	hacfKSpace_unordered_.print(std::cout);

	// new array
	hoNDArray<std::complex<float> > hacfTmp(vtDims_new);
	hacfTmp.fill(std::complex<float>(0.0, 0.0));
	#pragma omp parallel for
	for (size_t iR = 0; iR < hacfKSpace_unordered_.get_size(0); iR++) {
		for (size_t iL = static_cast<size_t>(iStartIndex); iL < hacfKSpace_unordered_.get_size(1); iL++) {
			for (size_t iC = 0; iC < hacfKSpace_unordered_.get_size(2); iC++) {
				hacfTmp(iR, iL-iStartIndex, iC) = hacfKSpace_unordered_(iR, iL, iC);
			}
		}
	}

	GINFO("kSpace deleted:\n");
	hacfTmp.print(std::cout);

	hacfKSpace_unordered_ = hacfTmp;

	return true;
}

bool CS_Retro_PopulationGadget::fCalcCentroids(int iNoGates)
{
	// get centroids
	float fNavMin, fNavMax;
	fNavMin = fNavMax = 0;

	switch (GlobalVar::instance()->iGatingMode_) {
	// percentile
	case 0:
		GINFO("get inhale/exhale borders by 10th and 90th percentile..\n");

		if (vNavInt_.size() > 0) {
			fNavMin = vNavInt_.at(std::min_element(vNavInt_.begin(), vNavInt_.end())-vNavInt_.begin());
			fNavMax = vNavInt_.at(std::max_element(vNavInt_.begin(), vNavInt_.end())-vNavInt_.begin());
		}

		GDEBUG("navigator min: %.1f, max: %.1f\n", fNavMin, fNavMax);

		if (fNavMin == fNavMax) {
			vfCentroids_.push_back(fNavMin);
		} else {
			// get histogram
			unsigned int iNumberBins = 256;
			std::vector<size_t> histogram = std::vector<size_t>(iNumberBins);

			// init 0
			for (size_t i = 0; i < iNumberBins; i++) {
				histogram.at(i) = 0;
			}

			for (size_t i = 0; i < vNavInt_.size(); i++) {
				unsigned int bin = static_cast<unsigned int>(std::floor(vNavInt_.at(i)/((fNavMax-fNavMin)/iNumberBins)));

				if (bin >= iNumberBins) {
					bin = iNumberBins - 1;
				}

				if (bin < 0) {
					bin = 0;
				}

				histogram.at(bin)++;
			}

			// find 90th percentile
			long long cumsum = 0;
			size_t counter = 0;
			while (cumsum < (.90*vNavInt_.size())) {
				cumsum += static_cast<long long>(histogram.at(counter++));
			}

			float f90p = counter*((fNavMax-fNavMin)/iNumberBins);

			// find 10th percentile
			counter = 0;
			cumsum = 0;
			while (cumsum < (.10*vNavInt_.size())) {
				cumsum += static_cast<long long>(histogram[counter++]);
			}

			float f10p = counter*((fNavMax-fNavMin)/iNumberBins);

			GINFO("get equally spaced gate position - 10th: %.2f, 90th: %.2f, nPhases: %i\n", f10p, f90p, iNoGates);

			// eqully spaced gate positions
			float fDistance = (f90p-f10p)/(iNoGates-1);
			for (long iI = 0; iI < iNoGates; iI++) {
				vfCentroids_.push_back(f10p + iI*fDistance);
			}

			// get tolerance of the gate positions
			float fTolerance = std::abs(vfCentroids_.at(0)-vfCentroids_.at(1))*fTolerance_/2.0;

			// fill tolerance vector
			for (int i = 0; i < iNoGates; i++) {
				vTolerance_.push_back(fTolerance);
			}
		}
		break;

	// k-means
	case 1:
		GERROR("reorder_kSpace: k-means gating is not implemented in this version!\n");

		return false;
		break;

	default:
		GERROR("reorder_kSpace: no gating mode specified!\n");

		return false;
		break;
	}

	return true;
}

void CS_Retro_PopulationGadget::calculate_weights(std::vector<float> &weights, const int population_mode, const int phase)
{
	for (size_t i = 0; i < weights.size(); i++) {
		switch (population_mode) {
		// closest & average (weights are only needed for tolerance window)
		case 0:
		case 1:
			weights.at(i) = abs(vNavInt_.at(i) - vfCentroids_.at(phase));
			break;

		// gauss
		case 3:
			weights.at(i) = 1/(vTolerance_.at(phase)*std::sqrt(2*M_PI)) * exp(-(std::pow(vNavInt_.at(i)-vfCentroids_.at(phase),2))/(2*(std::pow(vTolerance_.at(phase),2))));
			break;

		default:
			throw runtime_error("Selected population_mode unknown!\n");
		}
	}
}

void CS_Retro_PopulationGadget::get_populated_data(hoNDArray<std::complex<float> > &populated_data, const int population_mode, const hoNDArray<std::complex<float> > &unordered, const std::vector<size_t> &indices, const std::vector<float> &centroid_distances)
{
	// Take shortcut by closest mode if it does not really matter (only 1 entry)
	int mode = population_mode;
	if (indices.size() == 1) {
		mode = 0;
	}

	switch (mode) {
	// closest
	case 0: {
		// calculate min distance
		size_t index_min_this_dist = std::min_element(centroid_distances.begin(), centroid_distances.end())-centroid_distances.begin();
		size_t iIndexMinDist = indices.at(index_min_this_dist);

		// copy data into return array
		size_t unordered_measurement_offset = iIndexMinDist * unordered.get_size(0);
		#pragma omp parallel for
		for (size_t channel = 0; channel < populated_data.get_size(1); channel++) {
			size_t populated_channel_offset = channel * populated_data.get_size(0);
			size_t unordered_channel_offset = channel * unordered.get_size(0) * unordered.get_size(1);

			memcpy(populated_data.get_data_ptr() + populated_channel_offset, unordered.get_data_ptr() + unordered_channel_offset + unordered_measurement_offset, sizeof(std::complex<float>)*populated_data.get_size(0));
		}

	} break;

	// average
	case 1: {
		// pre-filling step with zeros may be omitted here because we set the data below (=) and do not just append (+=)

		// take all measurements into account
		for (size_t measurement = 0; measurement < indices.size(); measurement++) {
			size_t unordered_measurement_offset = indices.at(measurement) * unordered.get_size(0);

			#pragma omp parallel for
			for (size_t channel = 0; channel < populated_data.get_size(1); channel++) {
				size_t populated_channel_offset = channel * populated_data.get_size(0);
				size_t unordered_channel_offset = channel * unordered.get_size(0) * unordered.get_size(1);

				#pragma omp parallel for
				for (size_t kx_pixel = 0; kx_pixel < populated_data.get_size(0); kx_pixel++) {
					// append weighted value to return array
					populated_data.at(kx_pixel + populated_channel_offset) = unordered.at(kx_pixel + unordered_channel_offset + unordered_measurement_offset);
				}
			}
		}

		// calculate overall weight sum and divide by it (weighted addition must be 1 at end)
		float normalization_factor = indices.size();

		// only divide if possible
		if (normalization_factor != 0) {
			// divide all elements by weight sum
			populated_data /= normalization_factor;
		}
	} break;

	// gauss
	case 3: {
		// pre-fill return array with zeros (IMPORTANT STEP)
		populated_data.fill(std::complex<float>(0.0));

		// take all measurements into account
		for (size_t measurement = 0; measurement < indices.size(); measurement++) {
			size_t unordered_measurement_offset = indices.at(measurement) * unordered.get_size(0);

			#pragma omp parallel for
			for (size_t channel = 0; channel < populated_data.get_size(1); channel++) {
				size_t populated_channel_offset = channel * populated_data.get_size(0);
				size_t unordered_channel_offset = channel * unordered.get_size(0) * unordered.get_size(1);

				#pragma omp parallel for
				for (size_t kx_pixel = 0; kx_pixel < populated_data.get_size(0); kx_pixel++) {
					// append weighted value to return array
					populated_data.at(kx_pixel + populated_channel_offset) += centroid_distances.at(measurement)*unordered.at(kx_pixel + unordered_channel_offset + unordered_measurement_offset);
				}
			}
		}

		// calculate overall weight sum and divide by it (weighted addition must be 1 at end)
		float normalization_factor = std::accumulate(std::begin(centroid_distances), std::end(centroid_distances), 0.0, std::plus<float>());

		// only divide if possible
		if (normalization_factor != 0) {
			// divide all elements by weight sum
			populated_data /= normalization_factor;
		}
	} break;

	default:
		throw runtime_error("Selected population_mode unknown!\n");
	}
}

bool CS_Retro_PopulationGadget::fPopulatekSpace(int iNoGates)
{
	GINFO("--- populate k-space ---\n");

	// check population mode
	switch (GlobalVar::instance()->iPopulationMode_) {
	case 0:
		GINFO("Using closest mode\n");
		break;
	case 1:
		GINFO("Using average mode\n");

		break;
	case 2:
		GERROR("reorder_kSpace: population mode 'collect' not implemented in this version\n");

		return false;
		break;
	case 3:
		GINFO("Using gauss mode\n");
		break;
	default:
		GERROR("reorder_kSpace: no population mode specified!\n");

		return false;
		break;
	}

	// drecks mdh
	if (GlobalVar::instance()->vPE_.size() > hacfKSpace_unordered_.get_size(1)) {
		GDEBUG("A vPE_/vPA_ measurement is removed!\n");
		GlobalVar::instance()->vPE_.pop_back();
		GlobalVar::instance()->vPA_.pop_back();
	}

	GDEBUG("global PE: %i, PA: %i\n", GlobalVar::instance()->vPE_.size(), GlobalVar::instance()->vPA_.size());

	// loop over phases/gates
	//#pragma omp parallel for (parallelizing line loop is more efficient and keeps output prints in order)
	for (int iPh = 0; iPh < iNoGates; iPh++) {
		// get weights
		std::vector<float> vWeights(vNavInt_.size());
		calculate_weights(vWeights, GlobalVar::instance()->iPopulationMode_, iPh);

		GINFO("weights calculated - phase: %i\n", iPh);

		// loop over lines
		#pragma omp parallel for
		for (size_t iLine = 0; iLine < hacfKSpace_reordered_.get_size(1); iLine++) {
			// loop over partitions
			#pragma omp parallel for
			for (size_t iPar = 0; iPar < hacfKSpace_reordered_.get_size(2); iPar++) {
				// check if iLine was acquired and push Indices on vector
				std::vector<size_t> lIndices;
				for (size_t i = 0; i < GlobalVar::instance()->vPE_.size(); i++) {
					if (GlobalVar::instance()->vPE_.at(i) == iLine) {
						lIndices.push_back(i);
					}
				}

				// check iPar of the found acquisitions
				std::vector<size_t> lIndices2;
				for (size_t n = 0; n < lIndices.size(); n++) {
					if (GlobalVar::instance()->vPA_.at(lIndices.at(n)) == iPar) {
						// only take measurement into account when tolerance is okay
						if (vWeights.at(lIndices.at(n)) < vTolerance_.at(iPh)) {
							lIndices2.push_back(lIndices.at(n));
						}
					}
				}

				// if no index is in the vector --> continue
				if (lIndices2.size() > 0) {
					// get weights (if multiple lines were found)
					std::vector<float> vThisDist;
					vThisDist.clear();

					for (size_t i = 0; i < lIndices2.size(); i++) {
						vThisDist.push_back(vWeights.at(lIndices2.at(i)));
					}

					// populate the data
					hoNDArray<std::complex<float> > populated_data(hacfKSpace_unordered_.get_size(0), iNoChannels_);
					get_populated_data(populated_data, GlobalVar::instance()->iPopulationMode_, hacfKSpace_unordered_, lIndices2, vThisDist);

					// copy populated data into great reordered kspace
					#pragma omp parallel for
					for (int c = 0; c < iNoChannels_; c++) {
						size_t tOffset_reordered =
							iLine * hacfKSpace_reordered_.get_size(0)
							+ iPar * hacfKSpace_reordered_.get_size(1) * hacfKSpace_reordered_.get_size(0)
							+ iPh * hacfKSpace_reordered_.get_size(2) * hacfKSpace_reordered_.get_size(1) * hacfKSpace_reordered_.get_size(0)
							+ c * iNoGates*hacfKSpace_reordered_.get_size(2) * hacfKSpace_reordered_.get_size(1) * hacfKSpace_reordered_.get_size(0);

						size_t offset_populated = c * populated_data.get_size(0);

						memcpy(hacfKSpace_reordered_.get_data_ptr() + tOffset_reordered, populated_data.get_data_ptr() + offset_populated, sizeof(std::complex<float>)*populated_data.get_size(0));
					}
				}
			}
		}

		GINFO("kspace populated - phase: %i\n", iPh);
	}

	return true;
}

GADGET_FACTORY_DECLARE(CS_Retro_PopulationGadget)
